"""Save one fresh decoded AirPlay frame. This command sends no phone input."""
import argparse
import json
from pathlib import Path
from airwindows_frames import AirPlayReader, receiver_data_folder


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pid", type=int, required=True, help="PID of a receiver started with --frames")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--data-folder", action="store_true",
                        help="print the receiver's data folder (settings.ini, debug.txt, crashes) and exit")
    args = parser.parse_args()
    if args.data_folder:
        try:
            print(json.dumps({"pid": args.pid, "data_folder": str(receiver_data_folder(args.pid))}))
        except OSError as error:
            parser.exit(1, f"Cannot inspect receiver {args.pid}: {error}\n")
        return
    if not args.output:
        parser.error("--output is required")
    try:
        result = AirPlayReader(args.pid).save(args.output)
    except (OSError, ValueError, TimeoutError) as error:
        parser.exit(1, f"No fresh phone frame: {error}\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
