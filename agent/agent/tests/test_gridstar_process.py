"""gridstar_process：左上角图标背后那套「在哪、在不在跑、不在就拉起来」的判定。

进程表与启动都要 mock 掉，否则测试会真的去枚举本机进程、真的拉起一个
GridStar——测试不该碰运行环境，也不该在没有装 GridStar 的机器上失败。
记录文件也必须指向临时目录，不然每跑一次测试就改一次现场状态。
"""
import json
import os

import pytest

import gridstar_process as gp


class FakeProcess:
    def __init__(self, exit_code=None):
        self._exit_code = exit_code
        self.pid = 4242

    def poll(self):
        return self._exit_code


@pytest.fixture(autouse=True)
def sandbox_record(tmp_path, monkeypatch):
    """发现结果写到临时目录，别动真实现场。"""
    monkeypatch.setattr(gp, "RECORD_FILE", tmp_path / "gridstar.json")


@pytest.fixture
def exe(tmp_path, monkeypatch):
    """一个存在的假可执行文件，并让解析直接命中它。"""
    path = tmp_path / "GridStar.exe"
    path.write_bytes(b"")
    monkeypatch.setattr(gp, "resolve_executable", lambda: (path, "测试", [str(path)]))
    return path


# ── 进程名 ──────────────────────────────────────────────────────────
def test_target_names_fall_back_to_default(monkeypatch):
    monkeypatch.delenv(gp.PROCESS_ENV, raising=False)
    assert gp._target_names() == ("gridstar.exe",)


def test_target_names_parse_comma_and_semicolon(monkeypatch):
    monkeypatch.setenv(gp.PROCESS_ENV, " GridStar.exe ;gridstar_dev.exe,, ")
    assert gp._target_names() == ("gridstar.exe", "gridstar_dev.exe")


def test_running_pids_match_by_executable_name_only(monkeypatch):
    monkeypatch.delenv(gp.PROCESS_ENV, raising=False)
    monkeypatch.setattr(gp, "_processes", lambda: [
        (100, "python.exe"),
        (200, "GridStar.exe"),
        (300, "GridStar2.exe"),
    ])

    # 大小写不敏感，但文件名必须整体相等：GridStar2.exe 不是目标
    assert gp.running_pids() == [200]


@pytest.mark.skipif(os.name != "nt", reason="读真实进程映像路径只在 Windows 上存在")
def test_image_path_reads_own_executable():
    located = gp.image_path(os.getpid())

    assert located is not None
    assert located.name.lower() == "python.exe"


# ── 程序位置 ────────────────────────────────────────────────────────
def test_as_executable_accepts_directory_and_quotes(tmp_path):
    folder = tmp_path / "GS"
    folder.mkdir()

    assert gp._as_executable('"%s"' % folder) == folder / "GridStar.exe"
    assert gp._as_executable(str(tmp_path / "a" / "GridStar.exe")) == tmp_path / "a" / "GridStar.exe"


def test_resolve_prefers_env_override(tmp_path, monkeypatch):
    env_dir = tmp_path / "from-env"
    env_dir.mkdir()
    (env_dir / "GridStar.exe").write_bytes(b"")
    local = tmp_path / "local"
    local.mkdir()
    (local / "GridStar.exe").write_bytes(b"")
    monkeypatch.setattr(gp, "APP_ROOT", local)
    monkeypatch.setenv(gp.EXE_ENV, str(env_dir))

    path, source, _ = gp.resolve_executable()

    assert path == env_dir / "GridStar.exe"
    assert source == gp.EXE_ENV


def test_resolve_uses_recorded_path_over_sibling(tmp_path, monkeypatch):
    """上次记录比「同目录猜」可信：用户把程序搬到别处也认得住。"""
    recorded_dir = tmp_path / "recorded"
    recorded_dir.mkdir()
    recorded = recorded_dir / "GridStar.exe"
    recorded.write_bytes(b"")
    local = tmp_path / "local"
    local.mkdir()
    (local / "GridStar.exe").write_bytes(b"")
    monkeypatch.setattr(gp, "APP_ROOT", local)
    monkeypatch.delenv(gp.EXE_ENV, raising=False)
    gp.remember(recorded)

    path, source, _ = gp.resolve_executable()

    assert path == recorded
    assert source == "上次记录"


def test_resolve_does_not_need_any_env_var(tmp_path, monkeypatch):
    """换台机器不配任何环境变量也要能起来：程序就在本程序旁边。"""
    local = tmp_path / "bin"
    local.mkdir()
    beside = local / "GridStar.exe"
    beside.write_bytes(b"")
    monkeypatch.setattr(gp, "APP_ROOT", local)
    monkeypatch.delenv(gp.EXE_ENV, raising=False)
    monkeypatch.setattr(gp, "running_pids", lambda: [])

    path, source, checked = gp.resolve_executable()

    assert path == beside
    assert source == "同目录"
    assert any("同目录" in item for item in checked)


def test_resolve_reports_every_checked_candidate(tmp_path, monkeypatch):
    empty = tmp_path / "empty"
    empty.mkdir()
    monkeypatch.setattr(gp, "APP_ROOT", empty)
    monkeypatch.delenv(gp.EXE_ENV, raising=False)
    monkeypatch.setattr(gp, "running_pids", lambda: [])
    # 注册表里可能真的装了 GridStar（开发机上就是），这里要把环境变量隔开
    monkeypatch.setattr(gp, "_registry_paths", lambda: [])

    path, source, checked = gp.resolve_executable()

    assert path is None and source == ""
    assert checked, "找不到时必须说清楚检查了哪些位置"


def test_remember_and_recall_roundtrip(tmp_path):
    target = tmp_path / "Deep" / "GridStar.exe"

    gp.remember(target)

    payload = json.loads(gp.RECORD_FILE.read_text(encoding="utf-8"))
    assert payload["exe"] == str(target)
    assert gp._recalled_path() == target


def test_recalled_path_ignores_broken_record():
    gp.RECORD_FILE.write_text("{ 坏掉的记录", encoding="utf-8")

    assert gp._recalled_path() is None


# ── 启动 ────────────────────────────────────────────────────────────
def test_ensure_running_reuses_existing_instance(monkeypatch, exe):
    monkeypatch.setattr(gp, "_processes", lambda: [(777, "gridstar.exe")])
    monkeypatch.setattr(gp, "image_path", lambda pid: None)
    spawned = []
    monkeypatch.setattr(gp, "_spawn", lambda path: spawned.append(path))

    result = gp.ensure_running()

    assert result["running"] is True
    assert result["launched"] is False
    assert result["pids"] == [777]
    # 已经在跑就绝不再起第二个实例
    assert spawned == []


def test_ensure_running_remembers_path_of_running_instance(monkeypatch, tmp_path):
    real = tmp_path / "far away" / "GridStar.exe"
    monkeypatch.setattr(gp, "_processes", lambda: [(777, "gridstar.exe")])
    monkeypatch.setattr(gp, "image_path", lambda pid: real)

    gp.ensure_running()

    assert gp._recalled_path() == real


def test_ensure_running_launches_then_waits_for_real_process(monkeypatch, exe):
    calls = {"count": 0}

    def processes():
        calls["count"] += 1
        # 第一次枚举是启动前的判定，之后才「出现」新进程
        return [] if calls["count"] == 1 else [(8888, "GridStar.exe")]

    monkeypatch.setattr(gp, "_processes", processes)
    monkeypatch.setattr(gp, "_spawn", lambda path: FakeProcess())

    result = gp.ensure_running(timeout=2.0)

    assert result["launched"] is True
    assert result["running"] is True
    assert result["pids"] == [8888]
    assert "error" not in result
    assert gp._recalled_path() == exe


def test_ensure_running_reports_missing_executable(monkeypatch):
    checked = ["环境变量 GRIDSTAR_EXE: D:\\nowhere\\GridStar.exe"]
    monkeypatch.setattr(gp, "_processes", lambda: [])
    monkeypatch.setattr(gp, "resolve_executable", lambda: (None, "", checked))
    monkeypatch.setattr(gp, "_spawn", lambda path: pytest.fail("不该启动"))

    result = gp.ensure_running()

    assert result["running"] is False
    assert result["launched"] is False
    # toast 里不喷一长串路径，但排查用的 checked 必须原样带出
    assert gp.EXE_ENV in result["error"]
    assert result["checked"] == checked


def test_ensure_running_reports_early_exit(monkeypatch, exe):
    monkeypatch.setattr(gp, "_processes", lambda: [])
    monkeypatch.setattr(gp, "_spawn", lambda path: FakeProcess(exit_code=3))

    result = gp.ensure_running(timeout=2.0)

    assert result["launched"] is True
    assert result["running"] is False
    assert "退出码 3" in result["error"]


def test_ensure_running_admits_when_process_never_shows_up(monkeypatch, exe):
    monkeypatch.setattr(gp, "_processes", lambda: [])
    monkeypatch.setattr(gp, "_spawn", lambda path: FakeProcess())

    result = gp.ensure_running(timeout=0.3)

    # 提交了启动但没核实到：既不谎报已启动，也不谎报失败
    assert result["launched"] is True
    assert result["running"] is False
    assert "error" not in result
    assert "warning" in result


def test_spawn_uses_executable_directory_as_cwd(monkeypatch, tmp_path):
    recorded = {}
    monkeypatch.setattr(gp.subprocess, "Popen", lambda args, **kwargs: recorded.update(
        {"args": args, **kwargs}) or FakeProcess())
    path = tmp_path / "GridStar.exe"
    path.write_bytes(b"")

    gp._spawn(path)

    # 工作目录必须是程序自己那一层，否则同目录的 DLL 找不到
    assert recorded["cwd"] == str(path.parent)
    assert recorded["args"] == [str(path)]


def test_spawn_falls_back_to_runas_when_elevation_required(monkeypatch, tmp_path):
    """GridStar.exe 带 requireAdministrator 清单：被 740 拒掉时要改走 UAC。"""
    path = tmp_path / "GridStar.exe"
    path.write_bytes(b"")
    elevated = []

    def deny(args, **kwargs):
        error = OSError(740, "请求的操作需要提升。")
        error.winerror = 740
        raise error

    monkeypatch.setattr(gp.subprocess, "Popen", deny)
    monkeypatch.setattr(gp, "_shell_runas", lambda exe: elevated.append(exe))
    monkeypatch.setattr(gp.os, "name", "nt")

    launched = gp._spawn(path)

    assert elevated == [path]
    assert launched.elevated is True
    # 提权启动拿不到进程句柄，poll 只能如实说「不知道」
    assert launched.poll() is None


def test_ensure_running_waits_briefly_when_elevated(monkeypatch, exe):
    monkeypatch.setattr(gp, "_processes", lambda: [])
    monkeypatch.setattr(gp, "ELEVATED_WAIT_SECONDS", 0.3)
    monkeypatch.setattr(gp, "_spawn", lambda path: gp._Launched(None, elevated=True))

    result = gp.ensure_running(timeout=30.0)

    # 等的是人点 UAC，不该把 30 秒预算全耗在这上面
    assert result["launched"] is True
    assert result["elevated"] is True
    assert result["running"] is False
    assert "UAC" in result["warning"]