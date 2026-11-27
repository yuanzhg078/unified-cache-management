"""SGLang 0.5.20 hooks for the opt-in UCM layerwise PUT linker."""

from __future__ import annotations

from contextvars import ContextVar
from dataclasses import dataclass
from functools import wraps

import torch

_scheduled_save = ContextVar("ucm_scheduled_save", default=None)
_active_saver = ContextVar("ucm_active_saver", default=None)
_replaying_graph = ContextVar("ucm_replaying_prefill_graph", default=None)


@dataclass
class _ForwardSave:
    batch: object
    cache_put: _CachePut
    planned: bool = False
    completed: bool = False


def _selected(server_args):
    config = vars(server_args).get("_ucm_linker_config", {})
    if (
        config.get("linker_module_path") != "ucm.integration.sglang.ucm_direct_linker"
        or config.get("linker") != "UcmLayerWiseLinker"
    ):
        return False
    from sglang.srt.runtime_context import get_memory

    memory = get_memory()
    return (
        memory.enable_unified_cache_external_linker
        and memory.unified_cache_external_linker_backend == "dynamic"
        and not memory.enable_hierarchical_cache
    )


def _eligible_model(model_runner):
    from sglang.srt.mem_cache.allocator.paged import PagedTokenToKVPoolAllocator
    from sglang.srt.mem_cache.memory_pool import DSATokenToKVPool
    from sglang.srt.runtime_context import get_exec, get_parallel

    parallel = get_parallel()
    # Interleave gathers full Indexer and MLA KV on each CP rank before the
    # cache writes, including the TRT-LLM FP8 path's deferred KV gather.
    interleave_cp = (
        parallel.attn_cp_size > 1
        and parallel.enable_prefill_cp
        and parallel.cp_strategy == "interleave"
    )
    if (
        torch.device(model_runner.device).type != "cuda"
        or model_runner.is_draft_worker
        # The target setter runs before packed draft fragments are written.
        or model_runner.mtp_draft_device_pools
        or model_runner.server_args.nnodes != 1
        or model_runner.ps.pp_size != 1
        or (parallel.attn_cp_size != 1 and not interleave_cp)
        or type(model_runner.token_to_kv_pool_allocator)
        is not PagedTokenToKVPoolAllocator
        or not isinstance(model_runner.token_to_kv_pool, DSATokenToKVPool)
        or (not interleave_cp and get_exec().kernel.dsa_prefill_backend == "trtllm")
    ):
        return False
    from sglang.srt.models.deepseek_v2 import DeepseekV2DecoderLayer

    layers = [
        layer
        for layer in model_runner.model.modules()
        if isinstance(layer, DeepseekV2DecoderLayer)
    ]
    return bool(layers) and all(layer.self_attn.use_dsa for layer in layers)


class _CachePut:
    def __init__(self, cache, linker):
        self.cache = cache
        self.linker = linker

    def prepare(self, batch):
        from sglang.srt.mem_cache.hicache_storage import PoolName, PoolTransfer
        from sglang.srt.mem_cache.radix_cache import RadixKey
        from sglang.srt.mem_cache.utils import get_storage_hash_str

        page_size = self.cache.page_size
        requests, slices = [], []
        prefill_count = len(batch.reqs) - len(batch.decoding_reqs or ())
        for req, start, end in zip(
            batch.reqs[:prefill_count],
            batch.prefix_lens[:prefill_count],
            batch.seq_lens_cpu[:prefill_count].tolist(),
            strict=True,
        ):
            first = max(start, req.kv.cache_protected_len) // page_size
            key = RadixKey(
                req.get_fill_ids(),
                req.extra_key,
                cache_salt=req.cache_salt,
                is_bigram=self.cache.tree_core.is_eagle,
            )
            last = min(end, len(key)) // page_size
            if first >= last:
                continue
            hashes = get_storage_hash_str(
                key[: last * page_size], None, page_size=page_size
            )
            requests.append(hashes[first:last])
            slices.append(
                self.cache.req_to_token_pool.req_to_token[
                    req.kv.req_pool_idx, first * page_size : last * page_size
                ]
            )
        if not slices:
            return -1
        # Pinning needs the physical page IDs on the scheduler. Reuse this
        # single host snapshot when the offload worker builds the addresses.
        slots = torch.cat(slices).cpu().split([part.numel() for part in slices])
        return self.linker.begin_layerwise_save(
            [
                PoolTransfer(PoolName.KV, device_indices=part, keys=keys)
                for part, keys in zip(slots, requests, strict=True)
            ]
        )

    def check_events(self):
        cache, wrapper, linker = self.cache, self.cache.linker, self.linker
        minimum = torch.distributed.ReduceOp.MIN
        counts = torch.tensor(
            [
                wrapper.num_completed_loads(),
                wrapper.num_completed_offloads(),
                linker.num_completed_saves(),
            ],
            dtype=torch.int,
            device="cpu",
        )
        cache._all_reduce_attn_groups(counts, minimum)
        load_count, offload_count, save_count = counts.tolist()
        wrapper.drain_loads(load_count)
        results = wrapper.take_completed_offloads(
            offload_count
        ) + linker.take_completed_saves(save_count)
        if results:
            successes = torch.tensor(results, dtype=torch.int, device="cpu")
            cache._all_reduce_attn_groups(successes, minimum)
            wrapper.commit_completed_offloads(
                [bool(result) for result in successes[:offload_count].tolist()]
            )
            # Source pages can be freed after every rank finished, on either outcome.
            linker.commit_completed_saves(save_count)


def bind(cache, worker, linker):
    """Attach the request-time hooks after the registry constructs the linker."""
    from sglang.srt.mem_cache.hicache_storage import PoolName

    runner = worker.model_runner
    graph = runner.prefill_cuda_graph_runner
    if (
        not _eligible_model(runner)
        or set(linker.pools) != {PoolName.KV, PoolName.INDEXER}
        or (graph is not None and not getattr(graph, "_ucm_layerwise_put", False))
    ):
        return

    cache._ucm_layerwise_put = _CachePut(cache, linker)
    cache.check_hicache_events = cache._ucm_layerwise_put.check_events
    protected_size = cache.protected_size
    # The tree no longer counts pages it has returned to the allocator.
    cache.protected_size = lambda: protected_size() + linker.deferred_source_tokens()
    worker._ucm_layerwise_saver = linker.saver
    pool = runner.token_to_kv_pool
    set_kv = pool.set_mla_kv_buffer

    @wraps(set_kv)
    def set_mla_kv_buffer(layer, *args, **kwargs):
        result = set_kv(layer, *args, **kwargs)
        saver = _active_saver.get()
        if saver is not None and _replaying_graph.get() is None:
            saver.save_layer(layer.layer_id)
        return result

    pool.set_mla_kv_buffer = set_mla_kv_buffer

    allocator = cache.token_to_kv_pool_allocator
    release = allocator._release_page_ids

    def release_page_ids(*page_ids):
        if not linker.has_saved_pages():
            return release(*page_ids)
        ids = torch.cat(page_ids)
        allowed = linker.filter_free_pages(ids.tolist())
        if len(allowed) == ids.numel():
            return release(*page_ids)
        if allowed:
            release(ids.new_tensor(allowed))

    allocator._release_page_ids = release_page_ids
    linker.release_source_pages = lambda ids: release(
        torch.tensor(ids, dtype=torch.int64, device=allocator.device)
    )


def _patch_scheduler(scheduler, worker_cls):
    run = scheduler.Scheduler.run_batch
    resolve = scheduler.resolve_forward_inputs
    forward = worker_cls.forward_batch_generation

    @wraps(run)
    def run_batch(self, batch, *args, **kwargs):
        cache_put = getattr(self.tree_cache, "_ucm_layerwise_put", None)
        if (
            cache_put is None
            or batch.prefill_input_ids_cpu is None
            or not self.is_generation
            or (
                not self.enable_overlap
                and self.enable_pdmux
                and batch.forward_mode.is_split_prefill()
            )
        ):
            return run(self, batch, *args, **kwargs)
        batch.save_index = -1
        state = _ForwardSave(batch, cache_put)
        token = _scheduled_save.set(state)
        success = False
        try:
            result = run(self, batch, *args, **kwargs)
            success = state.completed
            return result
        finally:
            if batch.save_index >= 0:
                cache_put.linker.finish_forward(batch.save_index, success)
                batch.save_index = -1
            _scheduled_save.reset(token)

    @wraps(resolve)
    def resolve_forward_inputs(batch, *args, **kwargs):
        result = resolve(batch, *args, **kwargs)
        state = _scheduled_save.get()
        if state is not None and state.batch is batch and not state.planned:
            state.planned = True
            batch.save_index = state.cache_put.prepare(batch)
        return result

    @wraps(forward)
    def forward_batch_generation(self, batch, *args, **kwargs):
        state = _scheduled_save.get()
        saver = getattr(self, "_ucm_layerwise_saver", None)
        if (
            state is None
            or state.batch is not batch
            or batch.save_index < 0
            or saver is None
        ):
            return forward(self, batch, *args, **kwargs)
        saver.consumer_index = batch.save_index
        token = _active_saver.set(saver)
        try:
            result = forward(self, batch, *args, **kwargs)
            state.completed = True
            return result
        finally:
            saver.consumer_index = -1
            _active_saver.reset(token)

    scheduler.Scheduler.run_batch = run_batch
    scheduler.resolve_forward_inputs = resolve_forward_inputs
    worker_cls.forward_batch_generation = forward_batch_generation


def _patch_graph(runner_cls):
    from sglang.srt.model_executor.runner_backend.breakable_cuda_graph_backend import (
        BreakableCudaGraphBackend,
    )
    from sglang.srt.model_executor.runner_backend_utils.breakable_cuda_graph import (
        eager_on_graph,
    )

    capture, execute = runner_cls.capture, runner_cls.execute

    @wraps(capture)
    def capture_prefill(self, *args, **kwargs):
        self._ucm_layerwise_put = (
            _selected(self.model_runner.server_args)
            and isinstance(self.backend, BreakableCudaGraphBackend)
            and _eligible_model(self.model_runner)
        )
        if not self._ucm_layerwise_put:
            return capture(self, *args, **kwargs)

        from sglang.srt.models.deepseek_v2 import DeepseekV2DecoderLayer

        @eager_on_graph(True)
        def wait_for_layer(layer_id):
            replay = _replaying_graph.get()
            if replay is not None and replay[0] is self and replay[2] is not None:
                pool = self.model_runner.token_to_kv_pool
                replay[2].wait_until(layer_id - pool.start_layer)

        @eager_on_graph(True)
        def save_layer(layer_id):
            replay = _replaying_graph.get()
            if replay is not None and replay[0] is self and replay[1] is not None:
                replay[1].save_layer(layer_id)

        def wrap_attention(forward, layer_id):
            @wraps(forward)
            def attention(*args, **kwargs):
                wait_for_layer(layer_id)
                output = forward(*args, **kwargs)
                save_layer(layer_id)
                return output

            return attention

        originals = []
        try:
            for layer in self.model_runner.model.modules():
                if isinstance(layer, DeepseekV2DecoderLayer):
                    attn = layer.self_attn
                    originals.append((attn, vars(attn).get("forward")))
                    attn.forward = wrap_attention(attn.forward, attn.layer_id)
            return capture(self, *args, **kwargs)
        finally:
            for attn, original in originals:
                if original is None:
                    del attn.forward
                else:
                    attn.forward = original

    @wraps(execute)
    def execute_prefill(self, *args, **kwargs):
        if not getattr(self, "_ucm_layerwise_put", False):
            return execute(self, *args, **kwargs)
        pool = self.model_runner.token_to_kv_pool
        token = _replaying_graph.set(
            (self, _active_saver.get(), pool.layer_transfer_counter)
        )
        try:
            return execute(self, *args, **kwargs)
        finally:
            _replaying_graph.reset(token)

    runner_cls.capture = capture_prefill
    runner_cls.execute = execute_prefill


def apply():
    from sglang.srt.managers import scheduler, tp_worker

    initialize = scheduler.Scheduler.init_model_worker
    if getattr(initialize, "_ucm_layerwise_put", False):
        return

    @wraps(initialize)
    def init_model_worker(self, *args, **kwargs):
        if _selected(self.server_args) and not getattr(
            scheduler.Scheduler.run_batch, "_ucm_layerwise_put", False
        ):
            from sglang.srt.model_executor.runner.prefill_cuda_graph_runner import (
                PrefillCudaGraphRunner,
            )

            _patch_graph(PrefillCudaGraphRunner)
            _patch_scheduler(scheduler, tp_worker.TpModelWorker)
            scheduler.Scheduler.run_batch._ucm_layerwise_put = True
        return initialize(self, *args, **kwargs)

    init_model_worker._ucm_layerwise_put = True
    scheduler.Scheduler.init_model_worker = init_model_worker
