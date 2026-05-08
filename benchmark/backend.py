import socket, os, sys, threading

path = sys.argv[1]
try:
    os.unlink(path)
except FileNotFoundError:
    pass

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.bind(path)
os.chmod(path, 0o777)
s.listen(128)

def handle(conn):
    try:
        while True:
            data = conn.recv(65536)
            if not data:
                break
            conn.sendall(data)
    except:
        pass
    finally:
        conn.close()

while True:
    conn, _ = s.accept()
    threading.Thread(target=handle, args=(conn,), daemon=True).start()
