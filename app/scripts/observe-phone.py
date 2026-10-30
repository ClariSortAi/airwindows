"""Save one fresh decoded AirPlay frame. This command sends no phone input."""
import argparse
import json
from pathlib import Path
from airwindows_frames import AirPlayReader


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pid", type=int, required=True, help="PID of a receiver started with --frames")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        result = AirPlayReader(args.pid).save(args.output)
    except (OSError, ValueError, TimeoutError) as error:
        parser.exit(1, f"No fresh phone frame: {error}\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
