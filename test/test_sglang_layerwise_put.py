"""Exercise the layerwise PUT pipeline with CPU test doubles."""

import sys
from collections import deque
from concurrent.futures import Future
from contextvars import ContextVar
from dataclasses import dataclass, replace
from functools import wraps
from queue import Empty, Queue
from threading import Event, Thread
from types import SimpleNamespace
from unittest.mock import Mock, patch

import numpy as np
import pytest
from common.sglang_utils import adapter_module, linker_module, load_source

patch_module = load_source(
    "ucm/integration/sglang/patch/v0520/layerwise_put_patch.py",
    ContextVar=ContextVar,
    wraps=wraps,
)


@dataclass
class Transfer:
    name: str
    keys: list[str]
    device_indices: np.ndarray
    host_indices: np.ndarray | None = None


def kv(keys, first_page):
    return Transfer(
        "kv", list(keys), np.arange(first_page * 4, (first_page + len(keys)) * 4)
    )


@pytest.fixture
def pipeline():
    owner = SimpleNamespace(rank=0)
    gate = Event()
    submitted, waited, ordinary_dumps, freed = Queue(), [], [], []
    ready_events = Queue()
    fail_layers = set()

    def init(linker):
        linker.page_size = 4
        linker._tp_rank, linker._dump_ranks = owner.rank, (0,)
        linker._bundles = [SimpleNamespace(layers=(0, 1))]
        linker._closed = False
        linker._load_queue, linker._completed_loads = Queue(), Queue()
        linker._pending_loads, linker._lookup_progress, linker._load_results = (
            {},
            {},
            {},
        )
        linker._offload_queue, linker._offload_results = Queue(), Queue()
        linker.layer_done_counter = Mock()
        linker._expand = lambda transfers, **kwargs: [
            replace(item, host_indices=item.device_indices) for item in transfers
        ]
        linker._page_transfers = lambda transfers, **kwargs: tuple(transfers[0].keys)
        linker._partition_offload = lambda transfers: (
            transfers if owner.rank == 0 else []
        )
        linker._freeze_gc_once = lambda: None
        linker._record_transfer_events = lambda: [
            SimpleNamespace(synchronize=lambda: None)
        ]
        linker.release_source_pages = freed.extend

    def submit(prepared, layer, prerequisite_handle):
        task = (prepared[layer], layer)
        submitted.put(task)
        return [task], RuntimeError("partial submit") if layer in fail_layers else None

    def wait(tasks):
        for task in tasks:
            assert gate.wait(3)
            waited.append(task)

    with (
        patch.multiple(
            linker_module,
            create=True,
            Queue=Queue,
            Empty=Empty,
            deque=deque,
            Future=Future,
            PoolName=SimpleNamespace(KV="kv"),
            torch=SimpleNamespace(cat=np.concatenate),
            device_module=SimpleNamespace(Event=lambda: ready_events.get_nowait()),
        ),
        patch.object(linker_module.UcmDirectLinker, "__init__", init),
    ):
        linkers = []

        def start(rank=0, *, native=True):
            owner.rank = rank
            linker = linker_module.UcmLayerWiseLinker()
            prepare = Mock(side_effect=lambda keys: {0: keys, 1: keys})
            linker.adapter = SimpleNamespace(
                supports_transfer_prerequisite=native,
                prepare_offload=prepare,
                submit_prepared_offload_layer=submit,
                wait=wait,
                submit_offload=lambda keys, **_: ordinary_dumps.append(keys) or [],
                check=lambda _: True,
                close=Mock(),
            )
            linker._load_thread = Thread(target=linker._load_worker, daemon=True)
            linker._offload_thread = Thread(target=linker._offload_worker, daemon=True)
            linker._load_thread.start()
            linker._offload_thread.start()
            linkers.append(linker)
            return linker

        yield SimpleNamespace(
            start=start,
            gate=gate,
            submitted=submitted,
            waited=waited,
            ready_events=ready_events,
            fail_layers=fail_layers,
            ordinary=ordinary_dumps,
            freed=freed,
        )
        gate.set()
        for linker in linkers:
            closer = Thread(target=linker.close, daemon=True)
            closer.start()
            closer.join(3)
            assert not closer.is_alive()
            assert not linker.put_wait_thread.is_alive()


def queue_layers(linker, pipeline, index, *layers, event=None):
    for _ in layers:
        pipeline.ready_events.put(
            event
            or SimpleNamespace(cuda_event=1, record=lambda: None, query=lambda: True)
        )
    linker.saver.consumer_index = index
    for layer in layers:
        linker.saver.save_layer(layer)
    linker.saver.consumer_index = -1


def test_wait_thread_does_not_block_submissions_and_releases_sources(pipeline):
    linker = pipeline.start(native=False)
    ready, waiting = Event(), Event()
    wait = linker.adapter.wait

    def wait_for_tasks(tasks):
        waiting.set()
        wait(tasks)

    linker.adapter.wait = wait_for_tasks
    first = linker.begin_layerwise_save([kv("a", 1)])
    assert linker.offload([kv("a", 1)])
    queue_layers(
        linker,
        pipeline,
        first,
        0,
        event=SimpleNamespace(record=lambda: None, query=ready.is_set),
    )
    queue_layers(linker, pipeline, first, 1)
    try:
        assert pipeline.submitted.get(timeout=2) == (("a",), 1)
        assert waiting.wait(2)
    finally:
        ready.set()
    assert pipeline.submitted.get(timeout=2) == (("a",), 0)
    linker.finish_forward(first, True)
    second = linker.begin_layerwise_save([kv("c", 3)])
    queue_layers(linker, pipeline, second, 0, 1)
    linker.finish_forward(second, True)

    assert [pipeline.submitted.get(timeout=2) for _ in range(2)] == [
        (("c",), 0),
        (("c",), 1),
    ]
    assert pipeline.waited == []
    assert linker.filter_free_pages([1, 99]) == [99]
    assert linker.num_completed_saves() == 0
    assert linker._offload_results.empty()

    pipeline.gate.set()
    assert linker._offload_results.get(timeout=2) is True
    assert pipeline.ordinary == []
    assert linker.offload([kv("b", 2)])
    assert linker._offload_results.get(timeout=2) is True
    assert pipeline.ordinary == [("b",)]
    assert linker.save_plans[second].completion.result(timeout=2) is True
    assert linker.take_completed_saves(2) == [True, True]
    linker.commit_completed_saves(2)
    assert pipeline.freed == [1]
    assert not linker.get_layerwise_receipts(["a"])
    assert linker.get_layerwise_receipts(["c"])
    assert linker.adapter.prepare_offload.call_count == 2


def test_mixed_node_rejects_offload_without_dumping_missing_pages(pipeline):
    linker = pipeline.start()
    index = linker.begin_layerwise_save([kv(["covered"], 2)])
    mixed = [kv(["left", "covered", "right"], 1)]
    assert linker.offload(mixed) is False

    queue_layers(linker, pipeline, index, 0, 1)
    linker.finish_forward(index, True)
    pipeline.gate.set()
    assert linker.save_plans[index].completion.result(timeout=2) is True
    assert linker.offload(mixed) is False
    linker._offload_queue.join()
    assert pipeline.ordinary == []
    assert linker.num_completed_offloads() == 0


def test_partial_submit_waits_and_fails_the_whole_node(pipeline):
    linker = pipeline.start()
    pipeline.fail_layers.add(1)
    index = linker.begin_layerwise_save([kv("a", 1)])
    assert linker.offload([kv("a", 1)])
    queue_layers(linker, pipeline, index, 0, 1)
    linker.finish_forward(index, True)
    assert [pipeline.submitted.get(timeout=2) for _ in range(2)] == [
        (("a",), 0),
        (("a",), 1),
    ]
    assert linker._offload_results.empty()
    pipeline.gate.set()
    assert linker._offload_results.get(timeout=2) is False
    assert pipeline.waited == [(("a",), 0), (("a",), 1)]
    assert pipeline.ordinary == []
    linker.commit_completed_saves(1)
    assert linker.offload([kv("a", 1)])
    assert linker._offload_results.get(timeout=2) is False


def test_non_owner_checks_forward_and_layers_only(pipeline):
    linker = pipeline.start(rank=1)
    index = linker.begin_layerwise_save([kv("a", 1)])
    queue_layers(linker, pipeline, index, 0, 0, 1)
    linker.finish_forward(index, True)
    assert linker.save_plans[index].completion.result(timeout=2) is True

    index = linker.begin_layerwise_save([kv("b", 2)])
    queue_layers(linker, pipeline, index, 0)
    linker.finish_forward(index, True)
    assert linker.save_plans[index].completion.result(timeout=2) is False
    assert pipeline.submitted.empty()


def test_reset_reaps_pending_layers_before_releasing_pins(pipeline):
    linker = pipeline.start()
    index = linker.begin_layerwise_save([kv("a", 1)])
    receipt = linker.save_plans[index].completion
    queue_layers(linker, pipeline, index, 0)
    assert pipeline.submitted.get(timeout=2) == (("a",), 0)
    assert linker.filter_free_pages([1]) == []
    assert linker.deferred_source_tokens() == 4
    reset = Thread(target=linker.reset, daemon=True)
    reset.start()
    assert not receipt.done()
    pipeline.gate.set()
    reset.join(2)
    assert not reset.is_alive()
    assert receipt.result(timeout=2) is False
    assert pipeline.waited == [(("a",), 0)]
    assert pipeline.freed == [1]
    assert linker.deferred_source_tokens() == 0
    assert not linker.has_saved_pages()


def test_layer_dump_preserves_store_shards_and_partial_handles():
    store = SimpleNamespace(dump_data=Mock(return_value=1))
    adapter = adapter_module.UcmDirectStoreAdapter(
        connector_name="test",
        module_path=None,
        store_config={},
        identity={},
    )
    address = np.array([[[11, 12], [13, 14]], [[21, 22], [23, 24]]], dtype=np.uint64)
    transfer = adapter_module.UcmLayerTransfer(
        "dsa",
        "kv",
        ("indexer", "kv"),
        (3, 7),
        16,
        (8, 8),
        ("a", "b"),
        address,
        ((8, 0),) * 4,
    )
    adapter._transfer_layout = lambda _: adapter_module._TransferLayout(
        (),
        store,
        2,
        {3: (0,), 7: (1,)},
    )
    adapter.submit_offload([transfer])
    ids, indexes, rows = store.dump_data.call_args.args
    assert indexes == [0, 0, 1, 1]
    lazy = replace(
        transfer, addresses=None, address_builder=lambda shard: address[shard]
    )
    prepared = adapter.prepare_offload([lazy])
    store.dump_data.side_effect = [1, RuntimeError("failed second bundle")]
    tasks, error = adapter.submit_prepared_offload_layer(
        {7: prepared[7] * 2},
        7,
    )
    assert len(tasks) == 1
    assert isinstance(error, RuntimeError)
    layer_ids, layer_indexes, layer_rows = store.dump_data.call_args_list[-2].args
    assert layer_ids == ids[2:]
    assert layer_indexes == indexes[2:]
    np.testing.assert_array_equal(layer_rows, rows[2:])

    native = SimpleNamespace(Dump=Mock(return_value=42))
    encoded_ids = np.frombuffer(b"".join(layer_ids), dtype=np.uint8)
    io = adapter_module._PreparedPipelineTransfer(native, encoded_ids, 2, int, False)
    native_item = replace(prepared[7][0], prepared_io=io)
    tasks, error = adapter.submit_prepared_offload_layer({7: [native_item]}, 7, 99)
    assert error is None and tasks[0].task == 42
    native_ids, native_indexes, native_rows, prerequisite = native.Dump.call_args.args
    assert native_ids is encoded_ids and prerequisite == 99
    np.testing.assert_array_equal(native_indexes, layer_indexes)
    np.testing.assert_array_equal(native_rows, layer_rows)


def test_scheduler_sets_index_after_resolve_and_clears_on_exception():
    saver = SimpleNamespace(consumer_index=-1)
    original_resolve = Mock()

    class Worker:
        _ucm_layerwise_saver = saver

        def forward_batch_generation(self, batch):
            assert patch_module._active_saver.get() is saver
            assert saver.consumer_index == batch.save_index == 7
            raise RuntimeError("forward failed")

    class Scheduler:
        is_generation = True
        enable_overlap = False
        enable_pdmux = False

        def run_batch(self, batch):
            scheduler.resolve_forward_inputs(batch, None)
            assert batch.save_index == 7
            self.worker.forward_batch_generation(batch)

    scheduler = SimpleNamespace(
        Scheduler=Scheduler, resolve_forward_inputs=original_resolve
    )
    patch_module._patch_scheduler(scheduler, Worker)
    batch = SimpleNamespace(
        prefill_input_ids_cpu=object(),
        reqs=[SimpleNamespace(multimodal_inputs=object())],
        decoding_reqs=[object()],
        forward_mode=SimpleNamespace(name="MIXED"),
        spec_algorithm=SimpleNamespace(is_none=lambda: False),
    )
    linker = SimpleNamespace(finish_forward=Mock())

    def prepare(batch):
        original_resolve.assert_called_once_with(batch, None)
        assert batch.save_index == -1
        return 7

    owner = Scheduler()
    owner.worker = Worker()
    owner.tree_cache = SimpleNamespace(
        _ucm_layerwise_put=SimpleNamespace(linker=linker, prepare=prepare)
    )
    with pytest.raises(RuntimeError, match="forward failed"):
        owner.run_batch(batch)
    assert batch.save_index == saver.consumer_index == -1
    assert (
        patch_module._scheduled_save.get() is patch_module._active_saver.get() is None
    )
    linker.finish_forward.assert_called_once_with(7, False)


def test_breakable_hooks_capture_boundaries_and_use_live_replay_context(monkeypatch):
    callbacks = []
    actions = []

    def eager_on_graph(enabled):
        assert enabled

        def decorate(fn):
            def register(layer):
                callbacks.append((fn, layer))
                fn(layer)

            return register

        return decorate

    class Backend:
        pass

    class DecoderLayer:
        def __init__(self):
            self.self_attn = SimpleNamespace(
                layer_id=7, forward=lambda: actions.append("kv")
            )

    deps = {
        "model_executor.runner_backend.breakable_cuda_graph_backend": {
            "BreakableCudaGraphBackend": Backend
        },
        "model_executor.runner_backend_utils.breakable_cuda_graph": {
            "eager_on_graph": eager_on_graph
        },
        "models.deepseek_v2": {"DeepseekV2DecoderLayer": DecoderLayer},
    }
    for module, exports in deps.items():
        monkeypatch.setitem(
            sys.modules, f"sglang.srt.{module}", SimpleNamespace(**exports)
        )
    monkeypatch.setattr(patch_module, "_selected", lambda _: True)
    monkeypatch.setattr(patch_module, "_eligible_model", lambda _: True)

    class Runner:
        backend = Backend()

        def capture(self):
            layer.self_attn.forward()

        def execute(self):
            fn, layer_id = callbacks[0]
            fn(layer_id)
            actions.append("replayed kv")
            fn, layer_id = callbacks[1]
            fn(layer_id)

    layer = DecoderLayer()
    wait = lambda index: actions.append(("get", index))
    runner = Runner()
    runner.model_runner = SimpleNamespace(
        server_args=None,
        model=SimpleNamespace(modules=lambda: [layer]),
        token_to_kv_pool=SimpleNamespace(
            start_layer=7, layer_transfer_counter=SimpleNamespace(wait_until=wait)
        ),
    )
    patch_module._patch_graph(Runner)
    original = layer.self_attn.forward
    runner.capture()
    assert layer.self_attn.forward is original
    assert actions == ["kv"]
    saver = SimpleNamespace(
        save_layer=lambda layer_id: actions.append(("put", layer_id))
    )
    token = patch_module._active_saver.set(saver)
    try:
        runner.execute()
    finally:
        patch_module._active_saver.reset(token)
    assert actions == ["kv", ("get", 0), "replayed kv", ("put", 7)]
    assert patch_module._replaying_graph.get() is None


def test_mix_interleave_cp_prefill_accepts_both_dsa_setter_paths(monkeypatch):
    class PagedAllocator:
        pass

    class DSAPool:
        pass

    class DecoderLayer:
        self_attn = SimpleNamespace(use_dsa=True, skip_topk=True)

    parallel = SimpleNamespace(
        attn_cp_size=8, enable_prefill_cp=True, cp_strategy="interleave"
    )
    kernel = SimpleNamespace(dsa_prefill_backend="flashmla_kv")
    deps = {
        "mem_cache.allocator.paged": {"PagedTokenToKVPoolAllocator": PagedAllocator},
        "mem_cache.memory_pool": {"DSATokenToKVPool": DSAPool},
        "runtime_context": {
            "get_parallel": lambda: parallel,
            "get_exec": lambda: SimpleNamespace(kernel=kernel),
        },
        "configs.model_config": {"get_dsa_index_kpool": lambda _: 2},
        "models.deepseek_v2": {"DeepseekV2DecoderLayer": DecoderLayer},
    }
    for module, exports in deps.items():
        monkeypatch.setitem(
            sys.modules, f"sglang.srt.{module}", SimpleNamespace(**exports)
        )
    monkeypatch.setattr(
        patch_module.torch,
        "device",
        lambda _: SimpleNamespace(type="cuda"),
        raising=False,
    )
    runner = SimpleNamespace(
        device="cuda",
        is_draft_worker=False,
        mtp_draft_device_pools=(),
        server_args=SimpleNamespace(nnodes=1),
        ps=SimpleNamespace(pp_size=1),
        token_to_kv_pool_allocator=PagedAllocator(),
        token_to_kv_pool=DSAPool(),
        model=SimpleNamespace(modules=lambda: [DecoderLayer()]),
        model_config=SimpleNamespace(hf_config=object()),
    )
    assert patch_module._eligible_model(runner)
    kernel.dsa_prefill_backend = "trtllm"
    assert patch_module._eligible_model(runner)
    runner.mtp_draft_device_pools = (object(),)
    assert not patch_module._eligible_model(runner)
    runner.mtp_draft_device_pools = ()
    parallel.cp_strategy = "zigzag"
    assert not patch_module._eligible_model(runner)
