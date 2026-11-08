"""Run external commands, echoing them live only while ``${E2E_DEBUG}`` is set."""

import shlex
import subprocess
import threading

from robot.api import logger
from robot.api.deco import keyword, library
from robot.libraries.BuiltIn import BuiltIn
from robot.utils import timestr_to_secs


class LiveResult:
    """Exit status and captured output of one command, shaped like ``Process`` results."""

    def __init__(self, rc, stdout, stderr, timed_out):
        self.rc = rc
        self.stdout = stdout
        self.stderr = stderr
        self.timed_out = timed_out


@library(scope="GLOBAL", auto_keywords=False)
class LiveProcess:
    """Like ``Process.Run Process``, keeping stdout and stderr apart in the result.

    With ``robot --variable E2E_DEBUG:true`` every line reaches the console as it
    is printed. Otherwise the console stays quiet and a failing command is printed
    once, after it exits. ``log.html`` records the command either way.
    """

    def __init__(self):
        self._console = threading.Lock()

    @keyword
    def console(self, message):
        """Print ``message`` when ``${E2E_DEBUG}`` is set; a no-op otherwise."""
        if self._debug():
            self._write(message)

    @keyword
    def run_live(self, *command, cwd=None, env=None, timeout="5 min"):
        """Run ``command`` and return its rc, stdout and stderr; ``timeout`` kills it."""
        command = [str(part) for part in command]
        debug = self._debug()
        # Leading newline: the console line may still hold the test name.
        shown = f"$ {shlex.join(command)}"
        if debug:
            self._write(f"\n{shown}")
        process = subprocess.Popen(
            command,
            cwd=cwd,
            env=env,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            errors="replace",
            bufsize=1,
        )
        stdout, stderr = [], []
        readers = [
            threading.Thread(target=self._pump, args=(process.stdout, stdout, debug), daemon=True),
            threading.Thread(target=self._pump, args=(process.stderr, stderr, debug), daemon=True),
        ]
        for reader in readers:
            reader.start()

        timed_out = False
        try:
            rc = process.wait(timeout=timestr_to_secs(timeout))
        except subprocess.TimeoutExpired:
            timed_out = True
            process.kill()
            rc = process.wait()
        # A grandchild that inherited the pipes could keep them open after a kill.
        for reader in readers:
            reader.join(timeout=5)

        result = LiveResult(rc, self._text(stdout), self._text(stderr), timed_out)
        logger.info(f"{shown}\nrc={rc}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}")
        if not debug and (rc != 0 or timed_out):
            note = f"killed after {timeout}" if timed_out else f"rc {rc}"
            self._write(f"\n{shown}  ({note})\n{result.stdout}\n{result.stderr}".rstrip())
        return result

    def _debug(self):
        # ``--variable`` passes text, so accept the spellings a flag would use.
        value = BuiltIn().get_variable_value("${E2E_DEBUG}", False)
        return str(value).strip().lower() in ("1", "true", "yes")

    def _pump(self, stream, sink, debug):
        for line in stream:
            sink.append(line)
            if debug:
                self._write(f"  | {line.rstrip()}")
        stream.close()

    def _write(self, message):
        with self._console:
            logger.console(message)

    @staticmethod
    def _text(lines):
        return "".join(lines).removesuffix("\n")
