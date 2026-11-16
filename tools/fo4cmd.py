"""Send commands to the running game's fo4flatvr command pipe and print the replies.

  python fo4cmd.py ping
  python fo4cmd.py status
  python fo4cmd.py "console coc QASmoke" "capture G:/Fallout4ToVR/work/shots/qa.bmp"
"""
import sys
import time

PIPE = r"\\.\pipe\fo4flatvr"


def main(cmds):
    deadline = time.time() + 10
    while True:
        try:
            f = open(PIPE, "r+b", buffering=0)
            break
        except OSError as e:
            if time.time() > deadline:
                sys.exit(f"cannot open {PIPE}: {e} (is the game running with fo4flatvr loaded?)")
            time.sleep(0.2)
    with f:
        for c in cmds:
            f.write((c + "\n").encode())
            reply = b""
            while not reply.endswith(b"\n"):
                chunk = f.read(4096)
                if not chunk:
                    sys.exit("pipe closed")
                reply += chunk
            print(f"> {c}\n{reply.decode().rstrip()}")


if __name__ == "__main__":
    main(sys.argv[1:] or ["ping"])
