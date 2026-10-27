"""M3 retry, concurrency and bounded-stream scenarios. Logs contain no credentials."""
from email.utils import formatdate
import time
from http_fixture import CONTENT
from m1_fixture import M1Handler


class M3Handler(M1Handler):
    def handle_get(self,path,query):
        if not path.startswith("/m3/"):
            return super().handle_get(path,query)
        mode=path[4:];key=query.get("case",[mode])[0]
        with self.server.requests_lock:
            counts=getattr(self.server,"m3_counts",{})
            count=counts[key]=counts.get(key,0)+1;self.server.m3_counts=counts
            timings=getattr(self.server,"m3_timings",{})
            timings.setdefault(key,[]).append(time.monotonic());self.server.m3_timings=timings
        if mode.startswith("retry-"):
            status=int(mode[6:]);failures=int(query.get("failures",[1])[0])
            if count<=failures:
                after=query.get("after",[""])[0]
                if after=="date":
                    after=formatdate(time.time()+2,usegmt=True)
                return self.respond(status,b"must-not-write",[("Retry-After",after)] if after else [])
        if mode=="stream":
            size=int(query.get("bytes",[262144])[0]);size=min(max(size,0),128*1024*1024)
            self.send_response(200);self.send_header("Content-Length",str(size));self.send_header("ETag",'"stream-v1"')
            self.send_header("Connection","close");self.end_headers()
            block=bytes(i%251 for i in range(65536))
            for at in range(0,size,65536):
                self.wfile.write(block[:min(65536,size-at)]);self.wfile.flush()
                if query.get("slow",["0"])[0]=="1":time.sleep(0.001)
            self.close_connection=True;return
        if mode=="unknown":return self.respond(200,CONTENT,chunked=True)
        start=0
        if self.headers.get("Range"):
            start=int(self.headers["Range"].removeprefix("bytes=").removesuffix("-"))
            return self.respond(206,CONTENT[start:],[("Content-Range",f"bytes {start}-{len(CONTENT)-1}/{len(CONTENT)}")])
        if mode=="stall":return self.respond(200,CONTENT[:1024],length=len(CONTENT),stall=True)
        return self.respond(200,CONTENT)
