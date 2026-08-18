"""Reusable S-expression helpers for KiCad 10 file generation.

KiCad stores symbols (.kicad_sym), footprints (.kicad_mod), schematics (.kicad_sch)
and boards (.kicad_pcb) in Lisp S-expression text format. These helpers produce
deterministic output: same input always yields byte-identical output, which makes
the generator auditable and git-friendly.

Target KiCad version: 10.0 (symbol version 20251024, footprint version 20260206,
schematic/PCB use the matching version strings exported by KiCad 10).

Typical usage in a project generator:

    from kicad_s_expr import (
        symbol_lib_header, symbol_lib_footer, generic_symbol,
        write_generated,
    )

    units = [("U", [("1", "A", "input"), ("2", "Y", "output"), ...])]
    lines = symbol_lib_header("MyLib")
    lines += generic_symbol(name="SN74LVC541A", reference="U", value="...",
                            footprint="Package_SO:TSSOP-20_...",
                            units=units, body_half_width=15.24, property_y=15.24)
    lines += symbol_lib_footer()
    write_generated(Path("MyLib.kicad_sym"), "".join(l + "\n" for l in lines))

Always run the result through kicad-cli to validate syntax and geometry:

    kicad-cli sym export svg -o out/ MyLib.kicad_sym
"""

from __future__ import annotations

from pathlib import Path
import uuid as _uuid


# ---------------------------------------------------------------------------
# Format versions (KiCad 10)
# ---------------------------------------------------------------------------

KICAD_SYM_VERSION = "20251024"      # .kicad_symbol_lib / .kicad_sym
KICAD_MOD_VERSION = "20260206"      # .kicad_mod (footprint)
KICAD_SCH_VERSION = "20260306"      # .kicad_sch written by KiCad 10.0.3
KICAD_PCB_VERSION = "20260206"      # .kicad_pcb written by KiCad 10.0.3

GENERATOR_NAME = "kicad-schematic-pcb-skill"
GENERATOR_VERSION = "1.1"


# UUIDv5 keeps generated files stable across runs.  Every schematic must use a
# stable, artifact-specific seed (normally its project-relative path).  The
# counters are reset by schematic_header(), which also makes repeated calls to
# the same generator idempotent inside one Python process.
_UUID_NAMESPACE = _uuid.UUID("f36b0441-e14f-4ba6-9931-16c7f9809282")
_generation_seed = "schematic"
_uuid_index = 0
_power_ref_index = 0
_flag_ref_index = 0


def reset_generation_state(seed: str = "schematic") -> None:
    """Reset deterministic UUID/reference sequences for one generated artifact."""
    global _generation_seed, _uuid_index, _power_ref_index, _flag_ref_index
    _generation_seed = seed
    _uuid_index = 0
    _power_ref_index = 0
    _flag_ref_index = 0


def _next_uuid(kind: str) -> str:
    """Return the next stable UUID for *kind* in the current artifact."""
    global _uuid_index
    _uuid_index += 1
    name = f"{_generation_seed}:{_uuid_index}:{kind}"
    return str(_uuid.uuid5(_UUID_NAMESPACE, name))


def _next_power_reference(prefix: str) -> str:
    """Return a unique KiCad hidden reference such as #PWR0001/#FLG0001."""
    global _power_ref_index, _flag_ref_index
    if prefix == "PWR":
        _power_ref_index += 1
        index = _power_ref_index
    elif prefix == "FLG":
        _flag_ref_index += 1
        index = _flag_ref_index
    else:
        raise ValueError(f"unknown power reference prefix: {prefix!r}")
    return f"#{prefix}{index:04d}"


# ---------------------------------------------------------------------------
# Low-level formatting
# ---------------------------------------------------------------------------

def fmt(value: float) -> str:
    """Format a numeric coordinate/size the way KiCad expects.

    Strips trailing zeros and a dangling decimal point, and normalises -0 to 0.
    KiCad accepts arbitrary precision, but a canonical form keeps diffs clean.
    """
    text = f"{value:.4f}".rstrip("0").rstrip(".")
    return "0" if text in {"-0", ""} else text


def sexpr(*tokens: object) -> str:
    """Wrap tokens in parens, joining with spaces. Strings are quoted if needed."""
    parts = []
    for t in tokens:
        if isinstance(t, str) and t and t[0] not in '"(' and " " in t:
            parts.append(f'"{t}"')
        else:
            parts.append(str(t))
    return "(" + " ".join(parts) + ")"


# ---------------------------------------------------------------------------
# File headers / footers
# ---------------------------------------------------------------------------

def symbol_lib_header(lib_name: str | None = None) -> list[str]:
    """Opening of a .kicad_sym library file."""
    return [
        "(kicad_symbol_lib",
        f"\t(version {KICAD_SYM_VERSION})",
        f'\t(generator "{GENERATOR_NAME}")',
        f'\t(generator_version "{GENERATOR_VERSION}")',
    ]


def symbol_lib_footer() -> list[str]:
    return [")"]


def footprint_header(
    name: str,
    layer: str = "F.Cu",
    descr: str = "",
    tags: str = "",
) -> list[str]:
    """Opening of a .kicad_mod footprint file."""
    lines = [
        f'(footprint "{name}"',
        f"\t(version {KICAD_MOD_VERSION})",
        f'\t(generator "{GENERATOR_NAME}")',
        f'\t(generator_version "{GENERATOR_VERSION}")',
        f'\t(layer "{layer}")',
    ]
    if descr:
        lines.append(f'\t(descr "{descr}")')
    if tags:
        lines.append(f'\t(tags "{tags}")')
    return lines


def schematic_header(paper: str = "A3", seed: str = "schematic") -> list[str]:
    """Opening of a .kicad_sch file with deterministic state reset.

    Pass a stable, artifact-specific seed, for example ``Hardware/main.kicad_sch``.
    """
    reset_generation_state(seed)
    return [
        "(kicad_sch",
        f"\t(version {KICAD_SCH_VERSION})",
        f'\t(generator "{GENERATOR_NAME}")',
        f'\t(generator_version "{GENERATOR_VERSION}")',
        f'\t(uuid "{_next_uuid("sheet")}")',
        f'\t(paper "{paper}")',
    ]


def schematic_footer() -> list[str]:
    """Closing of a .kicad_sch file.

    KiCad 10 requires `(embedded_fonts no)` as the last token before the final `)`.
    Without it the file fails to load silently («Не удалось загрузить схему»).
    """
    return ["\t(embedded_fonts no)", ")"]


def sheet_instances(page: str = "1") -> list[str]:
    """Root-sheet page block with the nesting required by KiCad 10."""
    return [
        "\t(sheet_instances",
        '\t\t(path "/"',
        f'\t\t\t(page "{page}")',
        "\t\t)",
        "\t)",
    ]


# ---------------------------------------------------------------------------
# Property blocks
# ---------------------------------------------------------------------------

def property_block(
    name: str, value: str, x: float, y: float, hidden: bool = False,
    angle: int = 0, font_size: float = 1.27,
) -> list[str]:
    """A `(property …)` block, used in both symbols and footprints (schematic form)."""
    lines = [
        f'\t\t(property "{name}" "{value}"',
        f"\t\t\t(at {fmt(x)} {fmt(y)} {angle})",
    ]
    if hidden:
        lines.append("\t\t\t(hide yes)")
    lines.extend(
        [
            "\t\t\t(effects",
            f"\t\t\t\t(font (size {fmt(font_size)} {fmt(font_size)}))",
            "\t\t\t)",
            "\t\t)",
        ]
    )
    return lines


def footprint_property(
    name: str, value: str, x: float, y: float, layer: str,
    angle: int = 0, font_size: float = 1.0, thickness: float = 0.15,
    mirrored: bool = False,
) -> list[str]:
    """A `(property …)` block in the footprint-specific form (with layer)."""
    lines = [
        f'\t(property "{name}" "{value}"',
        f"\t\t(at {fmt(x)} {fmt(y)} {angle})",
        f'\t\t(layer "{layer}")',
        "\t\t(effects",
        f"\t\t\t(font (size {fmt(font_size)} {fmt(font_size)}) (thickness {fmt(thickness)}))",
    ]
    if mirrored:
        lines.append("\t\t\t(justify mirror)")
    lines += [
        "\t\t)",
        "\t)",
    ]
    return lines


# ---------------------------------------------------------------------------
# Symbol pins
# ---------------------------------------------------------------------------

# Allowed electrical types for pins (see references/symbol-design.md).
PIN_ELECTRICAL_TYPES = {
    "input", "output", "bidirectional", "tri_state", "passive",
    "power_in", "power_out", "open_collector", "open_emitter",
    "unconnected", "no_connect", "free", "power_flag",
}

# Allowed graphical styles for pins.
PIN_GRAPHICAL_STYLES = {"line", "inverted", "clock", "input_low", "clock_low",
                       "output_low", "edge_clock_high", "non_logic"}


def pin_block(
    number: str, name: str, electrical_type: str, x: float, y: float,
    angle: int, length: float = 2.54, style: str = "line",
    name_size: float = 1.0, number_size: float = 1.0,
) -> list[str]:
    """A single `(pin …)` block inside a `symbol <name>_<unit>_<de Morgan>_1` body.

    electrical_type: one of PIN_ELECTRICAL_TYPES. Use:
      - input / output / bidirectional for normal signal pins
      - passive for connectors and resistors (no ERC direction)
      - power_in / power_out for supply pins
      - tri_state for 3-state outputs (74xx541, 573, etc.)
      - open_collector for OD outputs (74LVC07, /INT, /NMI drivers)
      - no_connect for pins the user must leave open
    """
    if electrical_type not in PIN_ELECTRICAL_TYPES:
        raise ValueError(f"unknown pin electrical_type: {electrical_type!r}")
    if style not in PIN_GRAPHICAL_STYLES:
        raise ValueError(f"unknown pin style: {style!r}")
    return [
        f"\t\t\t(pin {electrical_type} {style}",
        f"\t\t\t\t(at {fmt(x)} {fmt(y)} {angle})",
        f"\t\t\t\t(length {fmt(length)})",
        f'\t\t\t\t(name "{name}" (effects (font (size {fmt(name_size)} {fmt(name_size)}))))',
        f'\t\t\t\t(number "{number}" (effects (font (size {fmt(number_size)} {fmt(number_size)}))))',
        "\t\t\t)",
    ]


# ---------------------------------------------------------------------------
# Symbol builder
# ---------------------------------------------------------------------------

def generic_symbol(
    name: str,
    reference: str,
    value: str,
    footprint: str,
    units: list[tuple[str, list[tuple[str, str, str]]]],
    body_half_width: float = 15.24,
    property_y: float = 15.24,
    datasheet: str = "",
    description: str = "",
    pin_names_offset: float = 0.762,
) -> list[str]:
    """Build a complete `(symbol …)` block for a .kicad_sym library.

    Each unit is a (label, pins) tuple where pins is a list of
    (number, name, electrical_type). Pins are split left/right automatically
    by half; for full control over pin placement use a custom builder instead.

    Use multiple units when a chip has clearly separated function blocks
    (e.g. FPGA banks, MCU GPIO vs power). For most 74xx logic one unit suffices.
    """
    out = [
        f'\t(symbol "{name}"',
        f"\t\t(pin_names (offset {fmt(pin_names_offset)}))",
        "\t\t(exclude_from_sim no)",
        "\t\t(in_bom yes)",
        "\t\t(on_board yes)",
        "\t\t(property \"Reference\" placeholder)",  # replaced below
    ]
    # Replace placeholder with proper property blocks.
    out = [
        f'\t(symbol "{name}"',
        f"\t\t(pin_names (offset {fmt(pin_names_offset)}))",
        "\t\t(exclude_from_sim no)",
        "\t\t(in_bom yes)",
        "\t\t(on_board yes)",
    ]
    out += property_block("Reference", reference, -body_half_width, property_y)
    out += property_block("Value", value, body_half_width, property_y)
    out += property_block("Footprint", footprint, 0, 0, hidden=True)
    if datasheet:
        out += property_block("Datasheet", datasheet, 0, 0, hidden=True)
    if description:
        out += property_block("Description", description, 0, 0, hidden=True)

    for unit_index, (label, pins) in enumerate(units, start=1):
        split = (len(pins) + 1) // 2
        left = pins[:split]
        right = pins[split:]
        max_side = max(len(left), len(right), 1)
        body_half_height = max(5.08, ((max_side - 1) * 2.54) / 2 + 2.54)
        out.extend(
            [
                f'\t\t(symbol "{name}_{unit_index}_1"',
                "\t\t\t(rectangle",
                f"\t\t\t\t(start -{fmt(body_half_width)} {fmt(body_half_height)})",
                f"\t\t\t\t(end {fmt(body_half_width)} -{fmt(body_half_height)})",
                "\t\t\t\t(stroke (width 0.254) (type default))",
                "\t\t\t\t(fill (type background))",
                "\t\t\t)",
                f'\t\t\t(text "{label}"',
                f"\t\t\t\t(at 0 {fmt(body_half_height - 1.27)} 0)",
                "\t\t\t\t(effects (font (size 0.8 0.8) (bold yes)))",
                "\t\t\t)",
            ]
        )
        left_start = ((len(left) - 1) * 2.54) / 2
        for index, (number, pin_name, etype) in enumerate(left):
            out += pin_block(number, pin_name, etype,
                             -body_half_width - 2.54, left_start - index * 2.54, 0)
        right_start = ((len(right) - 1) * 2.54) / 2
        for index, (number, pin_name, etype) in enumerate(right):
            out += pin_block(number, pin_name, etype,
                             body_half_width + 2.54, right_start - index * 2.54, 180)
        out.append("\t\t)")
    out.extend(["\t\t(embedded_fonts no)", "\t)"])
    return out


# ---------------------------------------------------------------------------
# Footprint primitives
# ---------------------------------------------------------------------------

def fp_line(
    start: tuple[float, float], end: tuple[float, float], layer: str, width: float,
) -> list[str]:
    """A graphical line on a footprint (silk/fab/courtyard/Edge.Cuts)."""
    return [
        "\t(fp_line",
        f"\t\t(start {fmt(start[0])} {fmt(start[1])})",
        f"\t\t(end {fmt(end[0])} {fmt(end[1])})",
        f"\t\t(stroke (width {fmt(width)}) (type solid))",
        f'\t\t(layer "{layer}")',
        "\t)",
    ]


def fp_rect(
    start: tuple[float, float], end: tuple[float, float], layer: str, width: float,
    fill: bool = False,
) -> list[str]:
    """A graphical rectangle."""
    line = [
        "\t(fp_rect",
        f"\t\t(start {fmt(start[0])} {fmt(start[1])})",
        f"\t\t(end {fmt(end[0])} {fmt(end[1])})",
        f"\t\t(stroke (width {fmt(width)}) (type solid))",
        f'\t\t(layer "{layer}")',
    ]
    line.append(f"\t\t(fill {'yes' if fill else 'no'})")
    line.append("\t)")
    return line


def fp_circle(
    center: tuple[float, float], end: tuple[float, float], layer: str, width: float,
) -> list[str]:
    """A graphical circle (center + a point on the circumference)."""
    return [
        "\t(fp_circle",
        f"\t\t(center {fmt(center[0])} {fmt(center[1])})",
        f"\t\t(end {fmt(end[0])} {fmt(end[1])})",
        f"\t\t(stroke (width {fmt(width)}) (type solid))",
        f'\t\t(layer "{layer}")',
        "\t\t(fill no)",
        "\t)",
    ]


def fp_text_user(
    text: str, x: float, y: float, layer: str, angle: int = 0,
    font_size: float = 1.0, thickness: float = 0.15,
) -> list[str]:
    """User text on a footprint (silkscreen labels, etc.)."""
    return [
        f'\t(fp_text user "{text}"',
        f"\t\t(at {fmt(x)} {fmt(y)} {angle})",
        f'\t\t(layer "{layer}")',
        f"\t\t(effects (font (size {fmt(font_size)} {fmt(font_size)}) (thickness {fmt(thickness)})))",
        "\t)",
    ]


# Allowed pad types and shapes.
PAD_TYPES = {"smd", "thru_hole", "connect", "np_thru_hole"}
PAD_SHAPES = {"circle", "rect", "oval", "roundrect", "custom"}


def pad(
    number: str | int, pad_type: str, shape: str,
    x: float, y: float, size: tuple[float, float],
    layers: list[str], drill: float | None = None,
    roundrect_rratio: float | None = None, net: tuple[str, int] | None = None,
    die_length: float | None = None, solder_mask_margin: float | None = None,
    padstack_layers: list[
        tuple[str, str, tuple[float, float], tuple[float, float] | None]
    ] | None = None,
) -> list[str]:
    """A `(pad …)` block.

    pad_type: smd / thru_hole / connect (edge card) / np_thru_hole.
    shape:    circle / rect / oval / roundrect / custom.
    layers:   e.g. ["F.Cu","F.Mask","F.Paste"] for SMD; ["*.Cu"] for thru-hole.
    drill:    required for thru_hole.
    net:      (net_name, net_number) — assign the pad to a net (for board use).
    padstack_layers: per-layer copper overrides as
                     (layer, shape, size, offset-from-hole).
    """
    if pad_type not in PAD_TYPES:
        raise ValueError(f"unknown pad type: {pad_type!r}")
    if shape not in PAD_SHAPES:
        raise ValueError(f"unknown pad shape: {shape!r}")
    if pad_type in {"thru_hole", "np_thru_hole"} and drill is None:
        raise ValueError(f"thru_hole pad {number} requires drill diameter")
    if padstack_layers is not None and pad_type != "thru_hole":
        raise ValueError("per-layer padstacks are only supported for thru_hole pads")
    if padstack_layers is not None:
        for _, layer_shape, _, _ in padstack_layers:
            if layer_shape not in PAD_SHAPES:
                raise ValueError(f"unknown padstack layer shape: {layer_shape!r}")

    lines = [
        f'\t(pad "{number}" {pad_type} {shape}',
        f"\t\t(at {fmt(x)} {fmt(y)})",
        f"\t\t(size {fmt(size[0])} {fmt(size[1])})",
    ]
    if drill is not None:
        lines.append(f"\t\t(drill {fmt(drill)})")
    if roundrect_rratio is not None and shape == "roundrect":
        lines.append(f"\t\t(roundrect_rratio {fmt(roundrect_rratio)})")
    if die_length is not None:
        lines.append(f"\t\t(die_length {fmt(die_length)})")
    if solder_mask_margin is not None:
        lines.append(f"\t\t(solder_mask_margin {fmt(solder_mask_margin)})")
    layers_str = " ".join(f'"{l}"' for l in layers)
    lines.append(f"\t\t(layers {layers_str})")
    if net is not None:
        lines.append(f'\t\t(net {net[1]} "{net[0]}")')
    if padstack_layers is not None:
        lines += [
            "\t\t(padstack",
            "\t\t\t(mode front_inner_back)",
            '\t\t\t(layer "Inner"',
            f"\t\t\t\t(shape {shape})",
            f"\t\t\t\t(size {fmt(size[0])} {fmt(size[1])})",
            "\t\t\t\t(zone_connect -1)",
            "\t\t\t)",
        ]
        for layer_name, layer_shape, layer_size, layer_offset in padstack_layers:
            lines += [
                f'\t\t\t(layer "{layer_name}"',
                f"\t\t\t\t(shape {layer_shape})",
                f"\t\t\t\t(size {fmt(layer_size[0])} {fmt(layer_size[1])})",
            ]
            if layer_offset is not None:
                lines.append(
                    f"\t\t\t\t(offset {fmt(layer_offset[0])} "
                    f"{fmt(layer_offset[1])})"
                )
            lines += [
                "\t\t\t\t(zone_connect -1)",
                "\t\t\t)",
            ]
        lines.append("\t\t)")
    lines.append("\t)")
    return lines


# ---------------------------------------------------------------------------
# Schematic primitives
# ---------------------------------------------------------------------------

def symbol_instance(
    lib_id: str, x: float, y: float, reference: str, value: str,
    footprint: str = "", instance_uuid: str | None = None,
    unit: int = 1, angle: int = 0, mirror: str = "",
) -> list[str]:
    """A `(symbol …)` instance placed on the schematic sheet."""
    if instance_uuid is None:
        instance_uuid = _next_uuid(f"symbol:{reference}")
    lines = [
        "\t(symbol",
        f'\t\t(lib_id "{lib_id}")',
        f"\t\t(at {fmt(x)} {fmt(y)} {angle})",
        f"\t\t(unit {unit})",
        "\t\t(exclude_from_sim no)",
        "\t\t(in_bom yes)",
        "\t\t(on_board yes)",
        "\t\t(dnp no)",
        f'\t\t(uuid "{instance_uuid}")',
    ]
    if mirror:
        lines.append(f"\t\t(mirror {mirror})")
    lines += property_block("Reference", reference, x, y - 5.08, angle=0)
    lines += property_block("Value", value, x, y + 5.08, angle=0)
    if footprint:
        lines += property_block("Footprint", footprint, x, y, hidden=True)
    lines.append("\t)")
    return lines


def wire(x1: float, y1: float, x2: float, y2: float) -> list[str]:
    """A `(wire (pts …))` segment."""
    return [
        "\t(wire (pts (xy %s %s) (xy %s %s))" % (fmt(x1), fmt(y1), fmt(x2), fmt(y2)),
        "\t\t(stroke (width 0) (type default))",
        f'\t\t(uuid "{_next_uuid("wire")}")',
        "\t)",
    ]


def label(name: str, x: float, y: float, angle: int = 0,
          shape: str = "input") -> list[str]:
    """A local `(label …)` — net name scoped to current sheet."""
    return [
        f'\t(label "{name}"',
        f"\t\t(at {fmt(x)} {fmt(y)} {angle})",
        "\t\t(effects (font (size 1.27 1.27)) (justify left bottom))",
        f'\t\t(uuid "{_next_uuid(f"label:{name}")}")',
        "\t)",
    ]


def global_label(
    name: str, x: float, y: float, angle: int = 0,
    shape: str = "input", autoplaced: bool = True,
) -> list[str]:
    """A `(global_label …)` — visible across all sheets of the project.

    ``autoplaced`` is retained for source compatibility.  KiCad 10 does not
    require an intersheet-reference property on a label, so no synthetic field
    is emitted here.
    """
    _ = autoplaced
    lines = [
        f'\t(global_label "{name}"',
        f"\t\t(shape {shape})",
        f"\t\t(at {fmt(x)} {fmt(y)} {angle})",
        "\t\t(effects (font (size 1.27 1.27)) (justify right bottom))",
        f'\t\t(uuid "{_next_uuid(f"global_label:{name}")}")',
    ]
    lines.append("\t)")
    return lines


def hierarchical_label(
    name: str, x: float, y: float, angle: int = 0, shape: str = "input",
) -> list[str]:
    """A `(hierarchical_label …)` — pins a signal up to the parent sheet."""
    return [
        f'\t(hierarchical_label "{name}"',
        f"\t\t(shape {shape})",
        f"\t\t(at {fmt(x)} {fmt(y)} {angle})",
        "\t\t(effects (font (size 1.27 1.27)) (justify right bottom))",
        f'\t\t(uuid "{_next_uuid(f"hierarchical_label:{name}")}")',
        "\t)",
    ]


def junction(x: float, y: float, diameter: float = 0) -> list[str]:
    """A `(junction …)` — solid dot at a 3+ wire meeting point."""
    lines = [
        "\t(junction",
        f"\t\t(at {fmt(x)} {fmt(y)})",
        f'\t\t(diameter {fmt(diameter)})',
        f'\t\t(color "197 0 0 1")',
        f'\t\t(uuid "{_next_uuid("junction")}")',
        "\t)",
    ]
    return lines


def no_connect(x: float, y: float) -> list[str]:
    """A `(no_connect …)` — explicit red cross on an unused pin (good ERC hygiene)."""
    return [
        "\t(no_connect",
        f"\t\t(at {fmt(x)} {fmt(y)})",
        f'\t\t(uuid "{_next_uuid("no_connect")}")',
        "\t)",
    ]


def title_block(
    title: str, date: str = "", rev: str = "", company: str = "",
    comments: list[str] | None = None,
) -> list[str]:
    """A `(title_block …)` with the standard KiCad 9/10 schema."""
    lines = [
        "\t(title_block",
        f'\t\t(title "{title}")',
        f'\t\t(date "{date}")',
        f'\t\t(rev "{rev}")',
    ]
    if company:
        lines.append(f'\t\t(company "{company}")')
    for i, c in enumerate(comments or [], start=1):
        lines.append(f'\t\t(comment {i} "{c}")')
    lines.append("\t)")
    return lines


# ---------------------------------------------------------------------------
# Power symbols (convenience)
# ---------------------------------------------------------------------------

POWER_SYMBOLS = {
    # name -> (lib_id, value, pin_number, pin_name)
    "+3V3":   ("power:+3V3",   "+3V3",   "1", "PWR"),
    "+5V":    ("power:+5V",    "+5V",    "1", "PWR"),
    "+12V":   ("power:+12V",   "+12V",   "1", "PWR"),
    "GND":    ("power:GND",    "GND",    "1", "GND"),
    "VBUS":   ("power:VBUS",   "VBUS",   "1", "PWR"),
    "VSYS":   ("power:VSYS",   "VSYS",   "1", "PWR"),
    "+3.3VA": ("power:+3.3VA", "+3.3VA", "1", "PWR"),
    "+1V2":   ("power:+1V2",   "+1V2",   "1", "PWR"),
}


def power_symbol(
    name: str, x: float, y: float, angle: int = 0,
    reference: str | None = None,
) -> list[str]:
    """Place a standard power port (GND/+3V3/+5V/…).

    The matching lib_symbol must be inlined in the schematic's `(lib_symbols …)`
    block. For a one-off use a power_flag to satisfy ERC's power-out requirement.
    """
    if name not in POWER_SYMBOLS:
        raise ValueError(f"unknown power symbol: {name!r}; add it to POWER_SYMBOLS")
    lib_id, value, num, pin_name = POWER_SYMBOLS[name]
    return symbol_instance(
        lib_id=lib_id, x=x, y=y,
        reference=reference or _next_power_reference("PWR"), value=value,
        instance_uuid=_next_uuid(f"power:{name}"), angle=angle,
    )


def power_flag(x: float, y: float, reference: str | None = None) -> list[str]:
    """A `power:PWR_FLAG` — marks a net as externally powered (silences ERC)."""
    return symbol_instance(
        lib_id="power:PWR_FLAG", x=x, y=y,
        reference=reference or _next_power_reference("FLG"), value="PWR_FLAG",
        instance_uuid=_next_uuid("power_flag"),
    )


# ---------------------------------------------------------------------------
# Output
# ---------------------------------------------------------------------------

def write_generated(path: Path, content: str) -> None:
    """Write text with UTF-8 + LF newlines (KiCad requires LF, not CRLF)."""
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8", newline="\n")
    print(f"generated {path} ({path.stat().st_size} bytes)")


# ---------------------------------------------------------------------------
# Self-test
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    # Generate a tiny library to verify the helpers parse in KiCad 10.
    here = Path(__file__).resolve().parent
    out_dir = here / "_smoketest"
    out_dir.mkdir(exist_ok=True)

    lines = symbol_lib_header("smoke")
    lines += generic_symbol(
        name="SMOKE_PART",
        reference="U",
        value="SMOKE_PART",
        footprint="Package_DIP:DIP-8_W7.62mm",
        datasheet="~",
        description="smoke test part",
        units=[("UNIT1", [
            ("1", "IN",  "input"),
            ("2", "GND", "power_in"),
            ("3", "VCC", "power_in"),
            ("4", "OUT", "tri_state"),
        ])],
        body_half_width=10.16,
        property_y=7.62,
    )
    lines += symbol_lib_footer()
    content = "\n".join(lines) + "\n"
    write_generated(out_dir / "smoke.kicad_sym", content)
    print("OK — run: kicad-cli sym export svg -o", out_dir, out_dir / "smoke.kicad_sym")
