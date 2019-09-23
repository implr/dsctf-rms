from BaseHTTPServer import HTTPServer, BaseHTTPRequestHandler
from SocketServer import ThreadingMixIn

FLAG = open("flag.txt", "r").read().strip()

class Handler(BaseHTTPRequestHandler):

    def do_GET(self):
        if self.path == '/flag':
            self.send_response(200)
            self.end_headers()
            self.wfile.write(FLAG + "\n")
        else:
            self.send_response(404)
            self.end_headers()

class ThreadedHTTPServer(ThreadingMixIn, HTTPServer):
    pass

if __name__ == '__main__':
    server = ThreadedHTTPServer(('127.0.0.1', 8000), Handler)
    server.serve_forever()
