"""Lobby end to end with the stand-in game (launcher/tools/fake_game.py): a server and launchers on this PC, Qt's offscreen
platform, loopback only, a throw-away database in <work dir>. Checks that a lobby's games stay open between matches of the
same two players and close when the players change (docs/LAUNCHER.md, "Lobby"; PROTOCOL.md, "Lobbies: the games stay").

    python -I tools\\lobby-e2e.py <work dir> same|pair|spectator [seconds a stand-in match lasts, default 4] [--real]

--real runs the real game instead of the stand-in (mk9\\out\\build\\win-amd64-release\\mk9.exe, windowed 960x540; Alpha's game
jabs through the fights so that they end): two real games on this PC, about 5 minutes for `same`. Not while someone plays.

same       Alpha hosts, Bravo joins, two matches, both leave: each launcher boots one game, the second match starts in the
           games parked after the first (the server's log says so).
pair       Alpha hosts and leaves after one match, Bravo stays for a second against Delta, who joins after Alpha left:
           Bravo's parked game closes at once and the second match boots new games on both PCs.
spectator  as `same` with Charlie watching: his game boots for each match.

Needs build\\launcher\\MK9Launcher.exe (launcher\\build.cmd) and Qt's offscreen platform plugin (external\\qt). Prints each
launcher's log lines that matter and OK or the checks that failed (exit code 1). Kills what it started; <work dir> is
the caller's to delete (it holds the logs).
"""
import os
import pathlib
import re
import socket
import subprocess
import sys
import time

REPO = pathlib.Path(__file__).resolve().parent.parent
LAUNCHER = REPO / "build" / "launcher" / "MK9Launcher.exe"
FAKE = REPO / "launcher" / "tools" / "fake_game.py"
PLUGINS = REPO / "external" / "qt" / "6.12.0" / "msvc2022_64" / "plugins" / "platforms"
PORT, UDP = 17787, 17788

REAL = "--real" in sys.argv
if REAL:
    sys.argv.remove("--real")
    FAKE = REPO / "mk9" / "out" / "build" / "win-amd64-release" / "mk9.exe"
if len(sys.argv) < 3 or sys.argv[2] not in ("same", "pair", "spectator"):
    raise SystemExit(__doc__)
W = pathlib.Path(sys.argv[1]).resolve()
scenario = sys.argv[2]
fake_seconds = sys.argv[3] if len(sys.argv) > 3 else "4"
W.mkdir(parents=True, exist_ok=True)

env = dict(os.environ, QT_QPA_PLATFORM="offscreen", QT_QPA_PLATFORM_PLUGIN_PATH=str(PLUGINS),
           MK9_DEV_PASSWORD="e2e-test-pw-1", MK9_FAKE_SECONDS=fake_seconds)  # throw-away accounts in a throw-away database
procs = []
WAIT = 900 if REAL else 150  # seconds a scenario may take


def start_server():
    out = open(W / "server.out", "w")
    p = subprocess.Popen([sys.executable, "-m", "server", "--plain", "--host", "127.0.0.1", "--port", str(PORT),
                          "--udp-port", str(UDP), "--db", str(W / "server.sqlite"), "--log-file", str(W / "server.log")],
                         cwd=REPO, stdout=out, stderr=subprocess.STDOUT)
    procs.append(p)
    for _ in range(100):
        try:
            socket.create_connection(("127.0.0.1", PORT), timeout=0.3).close()
            return
        except OSError:
            time.sleep(0.1)
    raise SystemExit("the server did not come up")


def launcher(name, game_port, *args, attack=False):
    d = W / name
    d.mkdir(parents=True, exist_ok=True)
    # UPnP off: a test must not open ports on the router; the game port differs per launcher (one PC)
    (d / "mk9-launcher.ini").write_text(
        "[games]\nmk9\\exe=" + str(FAKE).replace("\\", "/") + "\n"
        "[online]\ngame_port=%d\nupnp=false\nmatch_sound=false\n"
        "[dev]\nudp_loopback=true\n"
        "[display]\nfullscreen=false\nwindow_size=960x540\n"
        "[tools]\npython=%s\n" % (game_port, sys.executable.replace("\\", "/")))
    out = open(d / "launcher.out", "w")
    # the real game's fights end when one player jabs through them (its autopilot, MK9_AUTOPILOT_ATTACK=1), and its
    # autopilot picks the fighter under the cursor when the start line says "cursor" (players pick with their own
    # controllers otherwise): --character cursor puts that in the lobby's match
    penv = dict(env, MK9_AUTOPILOT_ATTACK="1") if attack and REAL else env
    if REAL:
        args = (*args, "--character", "cursor")
    procs.append(subprocess.Popen(
        [str(LAUNCHER), "--config", str(d / "mk9-launcher.ini"), "--server", "tcp://127.0.0.1:%d" % PORT,
         "--user", name, "--register", *args], env=penv, cwd=d, stdout=out, stderr=subprocess.STDOUT))


def log_of(name):
    f = W / name / "mk9-launcher.log"
    return f.read_text(errors="replace") if f.exists() else ""


def wait_for(cond, seconds):
    end = time.time() + seconds
    while time.time() < end:
        if cond():
            return True
        time.sleep(1)
    return False


try:
    start_server()
    if scenario in ("same", "spectator"):
        launcher("Alpha", 7011, "--lobby", "host", "--lobby-name", "e2e", "--lobby-matches", "2", "--leave-lobby", attack=True)
        time.sleep(1.5)
        launcher("Bravo", 7012, "--lobby", "join", "--lobby-name", "e2e", "--lobby-matches", "2", "--leave-lobby")
        if scenario == "spectator":
            launcher("Charlie", 7013, "--lobby", "watch", "--lobby-name", "e2e")
        finished = wait_for(lambda: all("parked game closed" in log_of(n) for n in ("Alpha", "Bravo")), WAIT)
    else:
        launcher("Alpha", 7011, "--lobby", "host", "--lobby-name", "e2e", "--lobby-matches", "1", "--leave-lobby", attack=True)
        time.sleep(1.5)
        launcher("Bravo", 7012, "--lobby", "join", "--lobby-name", "e2e", "--lobby-matches", "2", "--leave-lobby")
        wait_for(lambda: "cli: leaving lobby" in log_of("Alpha"), WAIT)
        time.sleep(1)  # the launcher's lobby command joins once: the place is free by now
        launcher("Delta", 7014, "--lobby", "join", "--lobby-name", "e2e", "--lobby-matches", "1", "--leave-lobby")
        finished = wait_for(lambda: "cli: lobby match 2 of 2 over" in log_of("Bravo")
                            and "cli: lobby match 1 of 1 over" in log_of("Delta"), WAIT)
    time.sleep(3)
finally:
    for p in procs:
        subprocess.run(["taskkill", "/F", "/T", "/PID", str(p.pid)], capture_output=True)

KEY = re.compile(r"Game mk9 starting|countdown|parked|main menu|Match over|Starting the match|closed|cli:|"
                 r"Waiting for the game|did not|rror|Watching|Done watching", re.I)
boots = {}
for name in ("Alpha", "Bravo", "Charlie", "Delta"):
    text = log_of(name)
    if not text:
        continue
    boots[name] = text.count("Game mk9 starting")
    print("=" * 20, name, "game boots:", boots[name])
    for line in text.splitlines():
        if KEY.search(line):
            print("  ", line[11:150])
server_log = (W / "server.log").read_text(errors="replace") if (W / "server.log").exists() else ""
print("=" * 20, "server")
for line in server_log.splitlines():
    if re.search(r"lobby|match .* go|RESULT", line):
        print("  ", line[:170])

failed = []


def check(ok, what):
    if not ok:
        failed.append(what)


check(finished, "the scenario did not finish in time")
check(server_log.count("RESULT match=") == 2, "two matches with a result")
reused = "in the games parked after the last match" in server_log
if scenario in ("same", "spectator"):
    check(boots.get("Alpha") == 1 and boots.get("Bravo") == 1, "each player's game booted once")
    check(reused, "the server started the second match in the parked games")
    check("parked game takes the match" in log_of("Alpha") and "parked game takes the match" in log_of("Bravo"),
          "both launchers took the match in the parked game")
    if scenario == "spectator":
        check(boots.get("Charlie") == 2, "the spectator's game booted for each match")
else:
    check(boots.get("Alpha") == 1 and boots.get("Bravo") == 2 and boots.get("Delta") == 1,
          "Bravo booted a new game for the match against Delta, the others one")
    check(not reused, "the server did not start the second match in parked games")
    check("No opponent to play again" in log_of("Bravo"), "Bravo's parked game closed when Alpha left")
print("OK" if not failed else "FAILED: " + "; ".join(failed))
sys.exit(1 if failed else 0)
