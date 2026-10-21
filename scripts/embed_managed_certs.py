"""Create ESP-IDF's generated certificate assembly files before PlatformIO compiles."""

from pathlib import Path
import re

Import("env")

project_dir = Path(env.subst("$PROJECT_DIR"))
build_dir = Path(env.subst("$BUILD_DIR"))

certificates = (
    ("espressif__esp_insights", "https_server.crt"),
    ("espressif__esp_insights", "mqtt_server.crt"),
    ("espressif__esp_rainmaker", "rmaker_mqtt_server.crt"),
    ("espressif__esp_rainmaker", "rmaker_claim_service_server.crt"),
    ("espressif__esp_rainmaker", "rmaker_ota_server.crt"),
)


def identifier(name: str) -> str:
    return re.sub(r"[^a-zA-Z0-9_]", "_", name)


def embedded_assembly(data_path: Path, symbol: str, is_text: bool) -> str:
    data = data_path.read_bytes()
    payload = data + (b"\0" if is_text else b"")
    lines = [f"/* Data converted from {data_path} */"]
    if is_text:
        lines.append("/* null byte appended */")
    lines.extend((".data", "#if !defined (__APPLE__) && !defined (__linux__)", ".section .rodata.embedded", "#endif"))

    def label(name: str, comment: str = "") -> None:
        suffix = f" /* {comment} */" if comment else ""
        lines.extend((f".global {name}", f"{name}:{suffix}"))

    label(symbol)
    label(f"_binary_{symbol}_start", "for objcopy compatibility")
    for offset in range(0, len(payload), 16):
        chunk = payload[offset : offset + 16]
        lines.append(".byte " + ", ".join(f"0x{byte:02x}" for byte in chunk))
    label(f"_binary_{symbol}_end", "for objcopy compatibility")
    label(f"{symbol}_length", "not including null byte" if is_text else "")
    lines.extend((f".long {len(data)}", "", "#if defined (__linux__)", '.section .note.GNU-stack,"",@progbits', "#endif", ""))
    return "\n".join(lines)


build_dir.mkdir(parents=True, exist_ok=True)
for component, filename in certificates:
    source = project_dir / "managed_components" / component / "server_certs" / filename
    # On a clean PlatformIO build, this pre-script can run before ESP-IDF's
    # component manager has populated managed_components. Keep the upstream
    # public CA bundle in certs/ as a deterministic fallback.
    if not source.is_file():
        source = project_dir / "certs" / filename
    if not source.is_file():
        continue
    output = build_dir / f"{filename}.S"
    output.write_text(embedded_assembly(source, identifier(source.name), True), encoding="ascii")
