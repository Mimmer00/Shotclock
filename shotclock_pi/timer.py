import time
import threading
import queue

STATE_IDLE  = 'IDLE'
STATE_START = 'START'
STATE_STOP  = 'STOP'

class ShotClock:
    def __init__(self):
        self.lock        = threading.Lock()
        self.event_queue = queue.Queue()
        self.state       = STATE_STOP
        self.seconds     = 80
        self.max_seconds = 80
        self._next_tick  = None

        t = threading.Thread(target=self._tick_loop, daemon=True)
        t.start()

    def cmd(self, command):
        if command == 'START':
            with self.lock:
                self.state      = STATE_START
                self._next_tick = time.monotonic() + 1.0  # Tick ab jetzt synchronisieren
                sec = self.seconds
            self.event_queue.put(('START', sec))

        elif command == 'STOP':
            with self.lock:
                self.state = STATE_STOP
                sec = self.seconds
            self.event_queue.put(('STOP', sec))

        elif command == 'RESET':
            with self.lock:
                self.state   = STATE_STOP
                self.seconds = self.max_seconds
                sec = self.seconds
            self.event_queue.put(('RESET', sec))

        elif command.startswith('SETMAX '):
            val = int(command.split()[1])
            with self.lock:
                self.max_seconds = val
                self.seconds     = val
                self.state       = STATE_STOP
            self.event_queue.put(('RESET', val))

    def status(self):
        with self.lock:
            return {
                'state':   self.state,
                'seconds': self.seconds,
                'max':     self.max_seconds,
            }

    def _tick_loop(self):
        while True:
            time.sleep(0.05)
            with self.lock:
                if self.state != STATE_START:
                    continue
                now = time.monotonic()
                if now < self._next_tick:
                    continue
                self._next_tick += 1.0
                self.seconds = max(0, self.seconds - 1)
                sec   = self.seconds
                state = self.state
                if self.seconds == 0:
                    self.state = STATE_STOP

            if state == STATE_START:
                # SYNC alle 10 Sekunden
                if sec % 10 == 0 and sec > 0:
                    self.event_queue.put(('SYNC', sec))

            if sec == 0:
                self.event_queue.put(('STOP', 0))
