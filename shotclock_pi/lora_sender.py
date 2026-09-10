import serial
import threading
import time
import queue

UART_PORT = '/dev/ttyACM0'
UART_BAUD = 115200

class LoraSender:
    def __init__(self, event_queue):
        self._queue = event_queue
        try:
            self._serial = serial.Serial(UART_PORT, UART_BAUD, timeout=1)
            print(f'[LORA] Serial offen: {UART_PORT}')
        except Exception as e:
            self._serial = None
            print(f'[LORA] Serial Fehler: {e}')

        t = threading.Thread(target=self._sender_loop, daemon=True)
        t.start()

    def _sender_loop(self):
        while True:
            try:
                cmd, sec = self._queue.get(timeout=1.0)
            except queue.Empty:
                continue

            packet = f'CMD:{cmd}:{sec}'

            if cmd == 'SYNC':
                self._send_once(packet)          # SYNC nur 1×
            else:
                for i in range(3):               # Zustandswechsel 3×
                    self._send_once(packet)
                    if i < 2:
                        time.sleep(0.2)          # 200ms Abstand

    def _send_once(self, packet):
        if not self._serial:
            return
        try:
            self._serial.write((packet + '\n').encode())
            self._serial.flush()
            print(f'[LORA] Gesendet: {packet}')
        except Exception as e:
            print(f'[LORA] Sendefehler: {e}')
