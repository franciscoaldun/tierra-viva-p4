# Puente de internet PC -> P4 por el cable USB (puerto HUSB del P4, red 192.168.7.x).
# El PC deja conexiones esperando en el puerto 5001 del P4; cuando el P4 necesita un sitio escribe
# "CONNECT host:443\n", este programa abre esa conexión a internet y pasa los bytes en los dos sentidos.
# El HTTPS lo cifra el P4 con su hardware: el PC sólo ve bytes cifrados.
# Todas las conexiones las abre el PC: Windows no pide firewall ni administrador.
#
# Uso:  python puente_internet.py        (se deja abierto; se reconecta solo si el P4 se reinicia)
import socket, threading, time, sys

P4, PUERTO = "192.168.7.1", 5001
ESPERANDO = 3                 # conexiones que se dejan listas a la vez
stats = {"tuneles": 0, "bytes": 0, "errores": 0}
lock = threading.Lock()


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


def tubo(a, b):
    try:
        while True:
            d = a.recv(65536)
            if not d:
                break
            b.sendall(d)
            with lock:
                stats["bytes"] += len(d)
    except OSError:
        pass
    for s in (a, b):
        try:
            s.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass


def trabajador():
    while True:
        try:
            p4 = socket.create_connection((P4, PUERTO), timeout=5)
            p4.settimeout(None)
            linea = b""
            while not linea.endswith(b"\n"):
                c = p4.recv(1)
                if not c:
                    raise OSError("el P4 cerró")
                linea += c
                if len(linea) > 300:
                    raise OSError("pedido raro")
            txt = linea.decode("ascii", "replace").strip()
            if not txt.startswith("CONNECT "):
                p4.close()
                continue
            host, _, port = txt[8:].rpartition(":")
            port = int(port)
            if port not in (80, 443):
                p4.sendall(b"ERR puerto no permitido\n")
                p4.close()
                continue
            try:
                up = socket.create_connection((host, port), timeout=10)
                up.settimeout(None)
            except OSError as e:
                p4.sendall(("ERR %s\n" % e).encode())
                p4.close()
                with lock:
                    stats["errores"] += 1
                continue
            p4.sendall(b"OK\n")
            with lock:
                stats["tuneles"] += 1
            log("P4 ->", host)
            t = threading.Thread(target=tubo, args=(up, p4), daemon=True)
            t.start()
            tubo(p4, up)
            t.join(timeout=30)
            up.close()
            p4.close()
        except OSError:
            time.sleep(1)


if __name__ == "__main__":
    log("puente de internet para el P4 (%s:%d). Ctrl+C para cerrar." % (P4, PUERTO))
    for _ in range(ESPERANDO):
        threading.Thread(target=trabajador, daemon=True).start()
    ultimo = None
    try:
        while True:
            time.sleep(30)
            with lock:
                s = (stats["tuneles"], stats["bytes"], stats["errores"])
            if s != ultimo:
                log("túneles %d · %.1f KB · errores %d" % (s[0], s[1] / 1024, s[2]))
                ultimo = s
    except KeyboardInterrupt:
        sys.exit(0)
