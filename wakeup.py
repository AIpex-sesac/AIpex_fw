import argparse
import grpc
import wakeup_pb2 as pb2
import wakeup_pb2_grpc as pb2_grpc
import sys
import socket
import subprocess
import shutil

def resolve_hostname(name: str) -> str:
    # try native getaddrinfo
    try:
        infos = socket.getaddrinfo(name, None)
        for info in infos:
            addr = info[4][0]
            # prefer IPv4 addresses for readability; return first
            if '.' in addr:
                return addr
        # if no IPv4, return first addr (likely IPv6)
        if infos:
            return infos[0][4][0]
    except Exception:
        pass

    # fallback: avahi-resolve -n <name>
    if shutil.which("avahi-resolve"):
        try:
            p = subprocess.run(["avahi-resolve", "-n", name], capture_output=True, text=True, timeout=2)
            out = p.stdout.strip()
            # expected: "name\tip"
            if out:
                parts = out.split()
                if len(parts) >= 2:
                    return parts[1].strip()
        except Exception:
            pass
    return name  # return original if cannot resolve

def send_wakeup(target: str, script_name: str, args: str, timeout: float = 5.0) -> int:
    channel = grpc.insecure_channel(target)
    stub = pb2_grpc.WakeUpServiceStub(channel)
    req = pb2.WakeUpRequest(script_name=script_name, args=args)
    try:
        resp = stub.TriggerScript(req, timeout=timeout)
        print(f"[send_wakeup] success={resp.success} pid={resp.process_id} msg='{resp.message}'")
        return 0 if resp.success else 2
    except grpc.RpcError as e:
        print(f"[send_wakeup] RPC failed: code={e.code()} msg={e.details()}", file=sys.stderr)
        return 1

def main():
    p = argparse.ArgumentParser(description="Send WakeUp TriggerScript RPC")
    p.add_argument("--target", "-t", default="AipexCB.local:50050", help="gRPC target (host:port) or hostname(.local)")
    p.add_argument("--script", "-s", default="wakeup", help="script_name to trigger")
    p.add_argument("--args", "-a", default="", help="args string for script")
    p.add_argument("--timeout", type=float, default=5.0, help="RPC timeout seconds")
    args = p.parse_args()

    # normalize target: if host:port given, resolve host part if it looks like a .local name
    host = args.target
    port = None
    if ":" in args.target:
        # split last ':' to allow IPv6 literals (not expected for .local but safe)
        h, pstr = args.target.rsplit(":", 1)
        host = h
        port = pstr
    if host.endswith(".local") or host.endswith(".local."):
        resolved = resolve_hostname(host)
        if resolved != host:
            target = f"{resolved}:{port or '50050'}"
        else:
            target = f"{host}:{port or '50050'}"
    else:
        # if no port provided, append default
        if port:
            target = args.target
        else:
            target = f"{host}:50050"

    rc = send_wakeup(target, args.script, args.args, timeout=args.timeout)
    sys.exit(rc)

if __name__ == "__main__":
    main()