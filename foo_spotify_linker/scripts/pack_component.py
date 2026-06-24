import argparse
import shutil
import zipfile
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--platform", default="x64")
    parser.add_argument("--configuration", default="Release")
    args = parser.parse_args()

    root = Path(__file__).resolve().parents[1]
    result = root / "_result"
    dll = result / f"{args.platform}_{args.configuration}" / "bin" / "foo_spotify_linker.dll"
    component = result / "foo_spotify_linker.fb2k-component"

    if not dll.exists():
        raise SystemExit(f"DLL が見つかりません: {dll}")

    result.mkdir(parents=True, exist_ok=True)
    if component.exists():
        component.unlink()

    temp_zip = component.with_suffix(".zip")
    with zipfile.ZipFile(temp_zip, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.write(dll, dll.name)

    shutil.move(str(temp_zip), str(component))
    print(component)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
