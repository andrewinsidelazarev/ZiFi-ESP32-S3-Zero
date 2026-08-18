#!/usr/bin/env python3
"""Deterministic KiCad 10 generator for the ZiFi ESP32-S3-Zero adapter.

Mechanical intent:
* Waveshare ESP32-S3-Zero is installed on two 1 x 9 pin rows.
* USB-C faces the outer top edge; the ceramic antenna faces the ESP-01 header.
* Each module contact has a 1.0 mm THT hole. Its B.Cu planar-soldering land
  is 5.6 x 1.8 mm and extends 1.0 mm outward; F.Cu remains 3.6 x 1.8 mm.
* A 4 x 2, 2.54 mm male header exits the bottom like an ESP-01/ESP-01S.
* Only GND, +3V3, UART_TX and UART_RX are connected.
"""

from __future__ import annotations

import json
import math
from pathlib import Path
import sys
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(Path(__file__).resolve().parent))

from kicad_s_expr import (  # noqa: E402
    fp_line,
    fp_rect,
    footprint_header,
    footprint_property,
    generic_symbol,
    global_label,
    no_connect,
    pad,
    schematic_footer,
    schematic_header,
    sheet_instances,
    symbol_lib_footer,
    symbol_lib_header,
    title_block,
    write_generated,
)


PROJECT = "Zifi_ESP32_Zero_Adapter"
LIB = "ZifiAdapter"
GENERATOR = "zifi_esp32_zero_adapter_generator"
GENERATOR_VERSION = "1.0"
NS = uuid.UUID("96447c86-5bf1-4c74-84f4-3b5a83425463")

SYM_DIR = ROOT / "Libraries" / "Symbols"
FP_DIR = ROOT / "Libraries" / "Footprints" / f"{LIB}.pretty"
REF_DIR = ROOT / "Documentation" / "References"

SCHEMATIC = ROOT / f"{PROJECT}.kicad_sch"
BOARD = ROOT / f"{PROJECT}.kicad_pcb"
PROJECT_FILE = ROOT / f"{PROJECT}.kicad_pro"


def uid(name: str) -> str:
    return str(uuid.uuid5(NS, name))


def q(text: str) -> str:
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


S3_PINS = [
    # Left side of the official front-view pinout, top to bottom.
    ("18", "5V", "power_in"),
    ("17", "GND", "power_in"),
    ("16", "3V3", "power_in"),
    ("15", "GPIO1", "bidirectional"),
    ("14", "GPIO2", "bidirectional"),
    ("13", "GPIO3", "bidirectional"),
    ("12", "GPIO4", "bidirectional"),
    ("11", "GPIO5", "bidirectional"),
    ("10", "GPIO6", "bidirectional"),
    # Right side of the official front-view pinout, top to bottom.
    ("1", "TX/GPIO43", "output"),
    ("2", "RX/GPIO44", "input"),
    ("3", "GPIO13", "bidirectional"),
    ("4", "GPIO12", "bidirectional"),
    ("5", "GPIO11", "bidirectional"),
    ("6", "GPIO10", "bidirectional"),
    ("7", "GPIO9", "bidirectional"),
    ("8", "GPIO8", "bidirectional"),
    ("9", "GPIO7", "bidirectional"),
]

ESP01_PINS = [
    # Top/inner row, viewed from the component side with the module above.
    ("1", "GND", "power_out"),
    ("2", "GPIO2/NC", "passive"),
    ("3", "GPIO0/NC", "passive"),
    ("4", "RX", "passive"),
    # Bottom/outer row is listed right-to-left here so generic_symbol draws
    # the physical rows consistently. Actual pad numbers remain ESP-01.
    ("8", "VCC_3V3", "power_out"),
    ("7", "RST/NC", "passive"),
    ("6", "EN/NC", "passive"),
    ("5", "TX", "passive"),
]

S3_USED = {"1": "UART_TX", "2": "UART_RX", "16": "+3V3", "17": "GND"}
ESP01_USED = {"1": "GND", "4": "UART_RX", "5": "UART_TX", "8": "+3V3"}

BOARD_LEFT = 88.0
BOARD_RIGHT = 112.0
BOARD_TOP = 80.0
BOARD_BOTTOM = 113.0
U1_X = 100.0
U1_Y = 94.25
J1_X = 100.0
J1_Y = 109.5
J1_INNER_Y = J1_Y - 1.27
J1_OUTER_Y = J1_Y + 1.27
S3_RIGHT_ROW_X = 7.62
S3_LEFT_ROW_X = -7.62
S3_THT_PAD_SIZE = (3.6, 1.8)
S3_PLANAR_PAD_SIZE = (5.6, 1.8)
S3_PAD_DRILL = 1.0
S3_PAD_OUTWARD_OFFSET = 1.0

# The +3V3 route has one deliberate 45-degree exception to the otherwise
# orthogonal model.  It passes between ESP-01 pin 4 and ESP32 pad 9.
ESP01_PIN4 = (J1_X + 3.81, J1_INNER_Y)
ESP32_PAD9_HOLE = (
    U1_X + S3_RIGHT_ROW_X,
    U1_Y - 10.16 + 8 * 2.54,
)
ESP32_PAD9_COPPER_CENTER = (
    ESP32_PAD9_HOLE[0] + S3_PAD_OUTWARD_OFFSET,
    ESP32_PAD9_HOLE[1],
)
PAD_END_RADIUS = 0.9
ESP32_PAD_HALF_STRAIGHT = (
    S3_PLANAR_PAD_SIZE[0] - S3_PLANAR_PAD_SIZE[1]
) / 2
POWER_TRACE_HALF_WIDTH = 0.5

# For a 45-degree line, x-y is constant.  Place that line halfway between
# the facing B.Cu boundaries: J1.4 is a 1.8 mm circle and U1.9 is a
# 5.6 x 1.8 mm horizontal capsule shifted 1.0 mm outward. This maximizes
# the minimum clearance
# to those two pads for the required angle.
_pin4_facing_boundary = (
    ESP01_PIN4[0] - ESP01_PIN4[1] + math.sqrt(2) * PAD_END_RADIUS
)
_pad9_facing_boundary = (
    ESP32_PAD9_COPPER_CENTER[0] - ESP32_PAD9_COPPER_CENTER[1]
    - math.sqrt(2) * PAD_END_RADIUS
    - ESP32_PAD_HALF_STRAIGHT
)
POWER_DIAGONAL_X_MINUS_Y = (
    _pin4_facing_boundary + _pad9_facing_boundary
) / 2
POWER_DIAGONAL_START = (
    ESP01_PIN4[0],
    ESP01_PIN4[0] - POWER_DIAGONAL_X_MINUS_Y,
)
# First 0.01 mm grid point whose following vertical segment does not reduce
# the optimized diagonal clearance to J1.4.
POWER_DIAGONAL_END = (106.19, 106.19 - POWER_DIAGONAL_X_MINUS_Y)

# Deterministic routes. KiCad's Y coordinate grows downwards.  Every
# direction sequence is a hard constraint, not a routing preference.
ROUTES = {
    "UART_TX": (
        (107.62, 84.09), (111.0, 84.09), (111.0, 112.3),
        (96.19, 112.3), (96.19, J1_OUTER_Y),
    ),
    "GND": ((92.38, 86.63), (96.19, 86.63), (96.19, J1_INNER_Y)),
    "UART_RX": ((107.62, 86.63), (103.81, 86.63), (103.81, J1_INNER_Y)),
    "+3V3": (
        (92.38, 89.17), (103.81, 89.17), POWER_DIAGONAL_START,
        POWER_DIAGONAL_END, (POWER_DIAGONAL_END[0], J1_OUTER_Y),
        (103.81, J1_OUTER_Y),
    ),
}
ROUTE_DIRECTIONS = {
    "UART_TX": ("right", "down", "left", "up"),
    "GND": ("right", "down"),
    "UART_RX": ("left", "down"),
    "+3V3": ("right", "down", "down-right-45", "down", "left"),
}
ROUTE_STYLE = {
    "UART_TX": (1, 0.35, "F.Cu"),
    "GND": (5, 0.8, "F.Cu"),
    "UART_RX": (8, 0.35, "F.Cu"),
    "+3V3": (11, 1.0, "B.Cu"),
}

U1_UUID = uid("schematic/U1")
J1_UUID = uid("schematic/J1")


def make_symbol_blocks() -> tuple[list[str], list[str]]:
    s3 = generic_symbol(
        name="ESP32-S3-Zero",
        reference="U",
        value="ESP32-S3-Zero",
        footprint=f"{LIB}:ESP32-S3-Zero_THT",
        datasheet="https://docs.waveshare.com/ESP32-S3-Zero",
        description="Waveshare ESP32-S3-Zero, 18 hybrid THT/planar carrier pads",
        units=[("ESP32-S3-Zero", S3_PINS)],
        body_half_width=15.24,
        property_y=13.97,
    )
    esp01 = generic_symbol(
        name="ESP-01_4x2",
        reference="J",
        value="ESP-01 4x2",
        footprint=f"{LIB}:ESP-01_4x2_Male",
        datasheet="https://aithinker-static.oss-cn-shenzhen.aliyuncs.com/docs/Specification/ESP-01S_specification.pdf",
        description="ESP-01/ESP-01S physical 4x2 header, 2.54 mm pitch",
        units=[("ESP-01 TOP VIEW", ESP01_PINS)],
        body_half_width=10.16,
        property_y=8.89,
    )
    return s3, esp01


def generate_symbol_library() -> None:
    s3, esp01 = make_symbol_blocks()
    lines = symbol_lib_header(LIB)
    lines += s3
    lines += esp01
    lines += symbol_lib_footer()
    write_generated(SYM_DIR / f"{LIB}.kicad_sym", "\n".join(lines) + "\n")


def generate_s3_footprint() -> None:
    lines = footprint_header(
        "ESP32-S3-Zero_THT",
        descr="Waveshare ESP32-S3-Zero hybrid THT/planar footprint; 18 x 23.5 mm; 15.24 mm row spacing",
        tags="ESP32 S3 Zero Waveshare hybrid THT planar pin header module",
    )
    lines += footprint_property("Reference", "U**", 0, -13.2, "F.SilkS")
    lines += footprint_property("Value", "ESP32-S3-Zero", 0, 13.2, "F.Fab")
    lines.append("\t(attr through_hole)")

    # Body and courtyard. Side silk is intentionally omitted because it would
    # cross the two 1 x 9 pin rows.
    lines += fp_rect((-9.0, -11.75), (9.0, 11.75), "F.Fab", 0.1)
    lines += fp_rect((-9.25, -12.0), (9.25, 12.0), "F.CrtYd", 0.05)
    lines += fp_line((-5.0, -11.75), (5.0, -11.75), "F.SilkS", 0.15)

    # Official row centres are 15.24 mm apart; pitch is 2.54 mm.
    # The 1.0 mm drills accept ordinary 2.54 mm male pin headers. B.Cu lands
    # are 5.6 x 1.8 mm and extend 1.0 mm outward for direct planar soldering;
    # F.Cu stays 3.6 x 1.8 mm so the approved routing remains untouched.
    # Emit pads in numeric order so the library and KiCad's normalized board
    # copy compare identically.
    for number_int in range(1, 10):
        number = str(number_int)
        y = -10.16 + (number_int - 1) * 2.54
        lines += pad(
            number,
            "thru_hole",
            "rect" if number == "1" else "oval",
            S3_RIGHT_ROW_X,
            y,
            S3_THT_PAD_SIZE,
            ["*.Cu", "*.Mask"],
            drill=S3_PAD_DRILL,
            padstack_layers=[
                (
                    "B.Cu",
                    "rect" if number == "1" else "oval",
                    S3_PLANAR_PAD_SIZE,
                    (S3_PAD_OUTWARD_OFFSET, 0),
                )
            ],
        )
    for number_int in range(10, 19):
        number = str(number_int)
        y = 10.16 - (number_int - 10) * 2.54
        lines += pad(
            number,
            "thru_hole",
            "oval",
            S3_LEFT_ROW_X,
            y,
            S3_THT_PAD_SIZE,
            ["*.Cu", "*.Mask"],
            drill=S3_PAD_DRILL,
            padstack_layers=[
                (
                    "B.Cu",
                    "oval",
                    S3_PLANAR_PAD_SIZE,
                    (-S3_PAD_OUTWARD_OFFSET, 0),
                )
            ],
        )

    lines += ["\t(embedded_fonts no)", ")"]
    write_generated(FP_DIR / "ESP32-S3-Zero_THT.kicad_mod", "\n".join(lines) + "\n")


def generate_esp01_footprint() -> None:
    lines = footprint_header(
        "ESP-01_4x2_Male",
        layer="B.Cu",
        descr="Bottom-mounted ESP-01/ESP-01S compatible 4 x 2 male header, 2.54 mm pitch; top-view pin map",
        tags="ESP-01 ESP-01S 4x2 2.54 male header bottom",
    )
    lines += footprint_property("Reference", "J**", 0, -3.6, "B.SilkS", mirrored=True)
    lines += footprint_property("Value", "ESP-01 4x2", 0, 3.6, "B.Fab", mirrored=True)
    lines.append("\t(attr through_hole)")
    lines += fp_rect((-5.08, -2.54), (5.08, 2.54), "B.Fab", 0.1)
    lines += fp_rect((-5.33, -2.79), (5.33, 2.79), "B.CrtYd", 0.05)
    lines += fp_line((-5.08, -2.54), (5.08, -2.54), "B.SilkS", 0.15)
    lines += fp_line((5.08, -2.54), (5.08, 2.54), "B.SilkS", 0.15)
    lines += fp_line((5.08, 2.54), (-5.08, 2.54), "B.SilkS", 0.15)
    lines += fp_line((-5.08, 2.54), (-5.08, -2.54), "B.SilkS", 0.15)

    # Keep explicit top-view physical positions while the component body and
    # documentation layers are on the bottom side.
    xs = [-3.81, -1.27, 1.27, 3.81]
    # Inner/top row: GND, GPIO2, GPIO0, RX.
    for number, x in zip(["1", "2", "3", "4"], xs):
        lines += pad(
            number,
            "thru_hole",
            "rect" if number == "1" else "circle",
            x,
            -1.27,
            (1.8, 1.8),
            ["*.Cu", "*.Mask"],
            drill=1.0,
        )
    # Outer/bottom row: TX, EN, RST, VCC.
    for number, x in zip(["5", "6", "7", "8"], xs):
        lines += pad(
            number,
            "thru_hole",
            "circle",
            x,
            1.27,
            (1.8, 1.8),
            ["*.Cu", "*.Mask"],
            drill=1.0,
        )
    lines += ["\t(embedded_fonts no)", ")"]
    write_generated(FP_DIR / "ESP-01_4x2_Male.kicad_mod", "\n".join(lines) + "\n")


def inline_lib_symbol(block: list[str], name: str) -> list[str]:
    result: list[str] = []
    replaced = False
    for line in block:
        current = line
        if not replaced and current == f'\t(symbol "{name}"':
            current = f'\t(symbol "{LIB}:{name}"'
            replaced = True
        result.append("\t" + current)
    if not replaced:
        raise RuntimeError(f"could not inline symbol {name}")
    return result


def schematic_instance(
    lib_id: str,
    reference: str,
    value: str,
    footprint: str,
    x: float,
    y: float,
    instance_uuid: str,
    pin_numbers: list[str],
) -> list[str]:
    lines = [
        "\t(symbol",
        f"\t\t(lib_id {q(lib_id)})",
        f"\t\t(at {x:g} {y:g} 0)",
        "\t\t(unit 1)",
        "\t\t(exclude_from_sim no)",
        "\t\t(in_bom yes)",
        "\t\t(on_board yes)",
        "\t\t(dnp no)",
        f"\t\t(uuid {q(instance_uuid)})",
        f"\t\t(property {q('Reference')} {q(reference)}",
        f"\t\t\t(at {x:g} {y - 16.51:g} 0)",
        "\t\t\t(effects (font (size 1.27 1.27)))",
        "\t\t)",
        f"\t\t(property {q('Value')} {q(value)}",
        f"\t\t\t(at {x:g} {y + 16.51:g} 0)",
        "\t\t\t(effects (font (size 1.27 1.27)))",
        "\t\t)",
        f"\t\t(property {q('Footprint')} {q(footprint)}",
        f"\t\t\t(at {x:g} {y:g} 0)",
        "\t\t\t(hide yes)",
        "\t\t\t(effects (font (size 1.27 1.27)))",
        "\t\t)",
    ]
    for number in pin_numbers:
        lines.append(f"\t\t(pin {q(number)} (uuid {q(uid(f'schematic/{reference}/pin/{number}'))}))")
    lines.append("\t)")
    return lines


def generate_schematic() -> None:
    s3, esp01 = make_symbol_blocks()
    lines = schematic_header("A4", seed=f"{PROJECT}.kicad_sch")
    lines += title_block(
        "ZiFi ESP32-S3-Zero to ESP-01 adapter",
        date="2026-07-30",
        rev="1.0",
        comments=[
            "Only GND, +3V3, UART_TX and UART_RX are connected",
            "Remove adapter from host before attaching USB-C",
        ],
    )
    lines.append("\t(lib_symbols")
    lines += inline_lib_symbol(s3, "ESP32-S3-Zero")
    lines += inline_lib_symbol(esp01, "ESP-01_4x2")
    lines.append("\t)")

    # Keep every pin endpoint on KiCad's 1.27 mm schematic grid.
    jx, jy = 69.85, 99.06
    ux, uy = 139.70, 99.06
    lines += schematic_instance(
        f"{LIB}:ESP-01_4x2",
        "J1",
        "ESP-01 4x2",
        f"{LIB}:ESP-01_4x2_Male",
        jx,
        jy,
        J1_UUID,
        ["1", "2", "3", "4", "5", "6", "7", "8"],
    )
    lines += schematic_instance(
        f"{LIB}:ESP32-S3-Zero",
        "U1",
        "ESP32-S3-Zero",
        f"{LIB}:ESP32-S3-Zero_THT",
        ux,
        uy,
        U1_UUID,
        [str(n) for n in range(1, 19)],
    )

    # J1 endpoints: left pins 1..4, right pins 8,7,6,5.
    j_left_x = jx - 12.70
    j_right_x = jx + 12.70
    # Positive local symbol Y is upward, therefore it subtracts from the
    # absolute KiCad sheet Y coordinate.
    j_y = {
        "1": jy - 3.81,
        "2": jy - 1.27,
        "3": jy + 1.27,
        "4": jy + 3.81,
        "8": jy - 3.81,
        "7": jy - 1.27,
        "6": jy + 1.27,
        "5": jy + 3.81,
    }
    lines += global_label("GND", j_left_x, j_y["1"], shape="bidirectional")
    lines += global_label("UART_RX", j_left_x, j_y["4"], shape="bidirectional")
    lines += global_label("+3V3", j_right_x, j_y["8"], angle=180, shape="bidirectional")
    lines += global_label("UART_TX", j_right_x, j_y["5"], angle=180, shape="bidirectional")
    for number in ["2", "3"]:
        lines += no_connect(j_left_x, j_y[number])
    for number in ["6", "7"]:
        lines += no_connect(j_right_x, j_y[number])

    # U1 endpoints: pads 18..10 on the left, pads 1..9 on the right.
    u_left_x = ux - 17.78
    u_right_x = ux + 17.78
    left_numbers = ["18", "17", "16", "15", "14", "13", "12", "11", "10"]
    right_numbers = ["1", "2", "3", "4", "5", "6", "7", "8", "9"]
    u_y: dict[str, float] = {}
    for index, number in enumerate(left_numbers):
        u_y[number] = uy - 10.16 + index * 2.54
    for index, number in enumerate(right_numbers):
        u_y[number] = uy - 10.16 + index * 2.54

    lines += global_label("GND", u_left_x, u_y["17"], shape="bidirectional")
    lines += global_label("+3V3", u_left_x, u_y["16"], shape="bidirectional")
    lines += global_label("UART_TX", u_right_x, u_y["1"], angle=180, shape="bidirectional")
    lines += global_label("UART_RX", u_right_x, u_y["2"], angle=180, shape="bidirectional")
    for number in left_numbers:
        if number not in S3_USED:
            lines += no_connect(u_left_x, u_y[number])
    for number in right_numbers:
        if number not in S3_USED:
            lines += no_connect(u_right_x, u_y[number])

    lines += sheet_instances()
    lines += schematic_footer()
    write_generated(SCHEMATIC, "\n".join(lines) + "\n")


def board_property(
    name: str,
    value: str,
    x: float,
    y: float,
    layer: str,
    key: str,
    hide: bool = False,
    angle: int = 0,
    mirrored: bool = False,
) -> list[str]:
    lines = [
        f"\t\t(property {q(name)} {q(value)}",
        f"\t\t\t(at {x:g} {y:g} {angle})",
        f"\t\t\t(layer {q(layer)})",
    ]
    if hide:
        lines.append("\t\t\t(hide yes)")
    lines += [
        f"\t\t\t(uuid {q(uid(key))})",
        "\t\t\t(effects",
        "\t\t\t\t(font (size 1 1) (thickness 0.15))",
    ]
    if mirrored:
        lines.append("\t\t\t\t(justify mirror)")
    lines += ["\t\t\t)", "\t\t)"]
    return lines


def board_pad(
    reference: str,
    number: str,
    pad_type: str,
    shape: str,
    x: float,
    y: float,
    sx: float,
    sy: float,
    layers: list[str],
    net: str | None = None,
    drill: float | None = None,
    roundrect: float | None = None,
    padstack_layers: list[
        tuple[str, str, tuple[float, float], tuple[float, float] | None]
    ] | None = None,
) -> list[str]:
    lines = [
        f"\t\t(pad {q(number)} {pad_type} {shape}",
        f"\t\t\t(at {x:g} {y:g})",
        f"\t\t\t(size {sx:g} {sy:g})",
    ]
    if drill is not None:
        lines.append(f"\t\t\t(drill {drill:g})")
    lines.append("\t\t\t(layers " + " ".join(q(layer_name) for layer_name in layers) + ")")
    if roundrect is not None:
        lines.append(f"\t\t\t(roundrect_rratio {roundrect:g})")
    if net is not None:
        lines.append(f"\t\t\t(net {q(net)})")
    lines.append(f"\t\t\t(uuid {q(uid(f'pcb/{reference}/pad/{number}'))})")
    if padstack_layers is not None:
        lines += [
            "\t\t\t(padstack",
            "\t\t\t\t(mode front_inner_back)",
            "\t\t\t\t(layer \"Inner\"",
            f"\t\t\t\t\t(shape {shape})",
            f"\t\t\t\t\t(size {sx:g} {sy:g})",
            "\t\t\t\t\t(zone_connect -1)",
            "\t\t\t\t)",
        ]
        for layer_name, layer_shape, layer_size, layer_offset in padstack_layers:
            lines += [
                f"\t\t\t\t(layer {q(layer_name)}",
                f"\t\t\t\t\t(shape {layer_shape})",
                f"\t\t\t\t\t(size {layer_size[0]:g} {layer_size[1]:g})",
            ]
            if layer_offset is not None:
                lines.append(
                    f"\t\t\t\t\t(offset {layer_offset[0]:g} "
                    f"{layer_offset[1]:g})"
                )
            lines += [
                "\t\t\t\t\t(zone_connect -1)",
                "\t\t\t\t)",
            ]
        lines.append("\t\t\t)")
    lines.append("\t\t)")
    return lines


def board_fp_line(reference: str, index: int, start: tuple[float, float],
                  end: tuple[float, float], layer: str, width: float) -> list[str]:
    return [
        "\t\t(fp_line",
        f"\t\t\t(start {start[0]:g} {start[1]:g})",
        f"\t\t\t(end {end[0]:g} {end[1]:g})",
        f"\t\t\t(stroke (width {width:g}) (type solid))",
        f"\t\t\t(layer {q(layer)})",
        f"\t\t\t(uuid {q(uid(f'pcb/{reference}/line/{index}'))})",
        "\t\t)",
    ]


def board_fp_rect(reference: str, index: int, start: tuple[float, float],
                  end: tuple[float, float], layer: str, width: float) -> list[str]:
    return [
        "\t\t(fp_rect",
        f"\t\t\t(start {start[0]:g} {start[1]:g})",
        f"\t\t\t(end {end[0]:g} {end[1]:g})",
        f"\t\t\t(stroke (width {width:g}) (type solid))",
        "\t\t\t(fill no)",
        f"\t\t\t(layer {q(layer)})",
        f"\t\t\t(uuid {q(uid(f'pcb/{reference}/rect/{index}'))})",
        "\t\t)",
    ]


def board_s3_footprint() -> list[str]:
    lines = [
        f"\t(footprint {q(f'{LIB}:ESP32-S3-Zero_THT')}",
        "\t\t(layer \"F.Cu\")",
        f"\t\t(uuid {q(uid('pcb/U1/footprint'))})",
        f"\t\t(at {U1_X:g} {U1_Y:g} 0)",
        "\t\t(descr \"Waveshare ESP32-S3-Zero hybrid THT/planar footprint; 18 x 23.5 mm; 15.24 mm row spacing\")",
        "\t\t(tags \"ESP32 S3 Zero Waveshare hybrid THT planar pin header module\")",
    ]
    lines += board_property("Reference", "U1", 0, -13.2, "F.SilkS", "pcb/U1/ref", hide=True)
    lines += board_property("Value", "ESP32-S3-Zero", 0, 13.2, "F.Fab", "pcb/U1/value")
    lines += [
        f"\t\t(path {q('/' + U1_UUID)})",
        "\t\t(sheetname \"/\")",
        f"\t\t(sheetfile {q(SCHEMATIC.name)})",
        "\t\t(attr through_hole)",
    ]
    lines += board_fp_rect("U1", 1, (-9.0, -11.75), (9.0, 11.75), "F.Fab", 0.1)
    lines += board_fp_rect("U1", 2, (-9.25, -12.0), (9.25, 12.0), "F.CrtYd", 0.05)
    lines += board_fp_line("U1", 1, (-5.0, -11.75), (5.0, -11.75), "F.SilkS", 0.15)
    for number_int in range(1, 10):
        number = str(number_int)
        lines += board_pad(
            "U1", number, "thru_hole", "rect" if number == "1" else "oval",
            S3_RIGHT_ROW_X, -10.16 + (number_int - 1) * 2.54,
            *S3_THT_PAD_SIZE, ["*.Cu", "*.Mask"],
            net=S3_USED.get(number), drill=S3_PAD_DRILL,
            padstack_layers=[
                (
                    "B.Cu",
                    "rect" if number == "1" else "oval",
                    S3_PLANAR_PAD_SIZE,
                    (S3_PAD_OUTWARD_OFFSET, 0),
                )
            ],
        )
    for number_int in range(10, 19):
        number = str(number_int)
        lines += board_pad(
            "U1", number, "thru_hole", "oval",
            S3_LEFT_ROW_X, 10.16 - (number_int - 10) * 2.54,
            *S3_THT_PAD_SIZE, ["*.Cu", "*.Mask"],
            net=S3_USED.get(number), drill=S3_PAD_DRILL,
            padstack_layers=[
                (
                    "B.Cu",
                    "oval",
                    S3_PLANAR_PAD_SIZE,
                    (-S3_PAD_OUTWARD_OFFSET, 0),
                )
            ],
        )
    lines += ["\t\t(embedded_fonts no)", "\t)"]
    return lines


def board_esp01_footprint() -> list[str]:
    lines = [
        f"\t(footprint {q(f'{LIB}:ESP-01_4x2_Male')}",
        "\t\t(layer \"B.Cu\")",
        f"\t\t(uuid {q(uid('pcb/J1/footprint'))})",
        f"\t\t(at {J1_X:g} {J1_Y:g} 0)",
        "\t\t(descr \"Bottom-mounted ESP-01/ESP-01S compatible 4 x 2 male header, 2.54 mm pitch; top-view pin map\")",
        "\t\t(tags \"ESP-01 ESP-01S 4x2 2.54 male header bottom\")",
    ]
    lines += board_property(
        "Reference", "J1", 0, -3.6, "B.SilkS", "pcb/J1/ref",
        hide=True, mirrored=True,
    )
    lines += board_property(
        "Value", "ESP-01 4x2", 0, 3.6, "B.Fab", "pcb/J1/value",
        mirrored=True,
    )
    lines += [
        f"\t\t(path {q('/' + J1_UUID)})",
        "\t\t(sheetname \"/\")",
        f"\t\t(sheetfile {q(SCHEMATIC.name)})",
        "\t\t(attr through_hole)",
    ]
    lines += board_fp_rect("J1", 1, (-5.08, -2.54), (5.08, 2.54), "B.Fab", 0.1)
    lines += board_fp_rect("J1", 2, (-5.33, -2.79), (5.33, 2.79), "B.CrtYd", 0.05)
    lines += board_fp_line("J1", 1, (-5.08, -2.54), (5.08, -2.54), "B.SilkS", 0.15)
    lines += board_fp_line("J1", 2, (5.08, -2.54), (5.08, 2.54), "B.SilkS", 0.15)
    lines += board_fp_line("J1", 3, (5.08, 2.54), (-5.08, 2.54), "B.SilkS", 0.15)
    lines += board_fp_line("J1", 4, (-5.08, 2.54), (-5.08, -2.54), "B.SilkS", 0.15)
    xs = [-3.81, -1.27, 1.27, 3.81]
    for number, x in zip(["1", "2", "3", "4"], xs):
        lines += board_pad(
            "J1", number, "thru_hole", "rect" if number == "1" else "circle",
            x, -1.27, 1.8, 1.8, ["*.Cu", "*.Mask"],
            net=ESP01_USED.get(number), drill=1.0,
        )
    for number, x in zip(["5", "6", "7", "8"], xs):
        lines += board_pad(
            "J1", number, "thru_hole", "circle",
            x, 1.27, 1.8, 1.8, ["*.Cu", "*.Mask"],
            net=ESP01_USED.get(number), drill=1.0,
        )
    lines += ["\t\t(embedded_fonts no)", "\t)"]
    return lines


def gr_line(index: int, start: tuple[float, float], end: tuple[float, float]) -> list[str]:
    return [
        "\t(gr_line",
        f"\t\t(start {start[0]:g} {start[1]:g})",
        f"\t\t(end {end[0]:g} {end[1]:g})",
        "\t\t(stroke (width 0.1) (type solid))",
        "\t\t(layer \"Edge.Cuts\")",
        f"\t\t(uuid {q(uid(f'pcb/edge/{index}'))})",
        "\t)",
    ]


def gr_text(
    index: int,
    text: str,
    x: float,
    y: float,
    size: float = 0.8,
    layer: str = "F.SilkS",
    mirrored: bool = False,
) -> list[str]:
    lines = [
        f"\t(gr_text {q(text)}",
        f"\t\t(at {x:g} {y:g} 0)",
        f"\t\t(layer {q(layer)})",
        f"\t\t(uuid {q(uid(f'pcb/text/{index}'))})",
        "\t\t(effects",
        f"\t\t\t(font (size {size:g} {size:g}) (thickness 0.12))",
    ]
    if mirrored:
        lines.append("\t\t\t(justify mirror)")
    lines += ["\t\t)", "\t)"]
    return lines


def segment(index: int, start: tuple[float, float], end: tuple[float, float],
            width: float, layer: str, net: str) -> list[str]:
    return [
        "\t(segment",
        f"\t\t(start {start[0]:g} {start[1]:g})",
        f"\t\t(end {end[0]:g} {end[1]:g})",
        f"\t\t(width {width:g})",
        "\t\t(locked yes)",
        f"\t\t(layer {q(layer)})",
        f"\t\t(net {q(net)})",
        f"\t\t(uuid {q(uid(f'pcb/segment/{index}'))})",
        "\t)",
    ]


def route_segments(net: str) -> list[str]:
    first_index, width, layer = ROUTE_STYLE[net]
    points = ROUTES[net]
    lines: list[str] = []
    for offset, (start, end) in enumerate(zip(points, points[1:])):
        lines += segment(first_index + offset, start, end, width, layer, net)
    return lines


def via(index: int, at: tuple[float, float], net: str) -> list[str]:
    return [
        "\t(via",
        f"\t\t(at {at[0]:g} {at[1]:g})",
        "\t\t(size 0.8)",
        "\t\t(drill 0.4)",
        "\t\t(layers \"F.Cu\" \"B.Cu\")",
        "\t\t(locked yes)",
        f"\t\t(net {q(net)})",
        f"\t\t(uuid {q(uid(f'pcb/via/{index}'))})",
        "\t)",
    ]


def generate_board() -> None:
    lines = [
        "(kicad_pcb",
        "\t(version 20260206)",
        f"\t(generator {q(GENERATOR)})",
        f"\t(generator_version {q(GENERATOR_VERSION)})",
        "\t(general (thickness 1.6) (legacy_teardrops no))",
        "\t(paper \"A4\")",
        "\t(title_block",
        "\t\t(title \"ZiFi ESP32-S3-Zero to ESP-01 adapter\")",
        "\t\t(date \"2026-07-30\")",
        "\t\t(rev \"1.0\")",
        f"\t\t(comment 1 \"24 x {BOARD_BOTTOM - BOARD_TOP:g} mm; two-layer; bottom-mounted ESP-01-compatible 4x2 header\")",
        "\t\t(comment 2 \"Only GND, +3V3, UART_TX and UART_RX are connected\")",
        "\t)",
        "\t(layers",
        "\t\t(0 \"F.Cu\" signal)",
        "\t\t(2 \"B.Cu\" signal)",
        "\t\t(9 \"F.Adhes\" user \"F.Adhesive\")",
        "\t\t(11 \"B.Adhes\" user \"B.Adhesive\")",
        "\t\t(13 \"F.Paste\" user)",
        "\t\t(15 \"B.Paste\" user)",
        "\t\t(5 \"F.SilkS\" user \"F.Silkscreen\")",
        "\t\t(7 \"B.SilkS\" user \"B.Silkscreen\")",
        "\t\t(1 \"F.Mask\" user)",
        "\t\t(3 \"B.Mask\" user)",
        "\t\t(17 \"Dwgs.User\" user \"User.Drawings\")",
        "\t\t(19 \"Cmts.User\" user \"User.Comments\")",
        "\t\t(21 \"Eco1.User\" user \"User.Eco1\")",
        "\t\t(23 \"Eco2.User\" user \"User.Eco2\")",
        "\t\t(25 \"Edge.Cuts\" user)",
        "\t\t(27 \"Margin\" user)",
        "\t\t(31 \"F.CrtYd\" user \"F.Courtyard\")",
        "\t\t(29 \"B.CrtYd\" user \"B.Courtyard\")",
        "\t\t(35 \"F.Fab\" user)",
        "\t\t(33 \"B.Fab\" user)",
        "\t)",
        "\t(setup",
        "\t\t(pad_to_mask_clearance 0)",
        "\t\t(allow_soldermask_bridges_in_footprints no)",
        "\t\t(tenting (front yes) (back yes))",
        "\t\t(covering (front no) (back no))",
        "\t\t(plugging (front no) (back no))",
        "\t\t(capping no)",
        "\t\t(filling no)",
        "\t)",
    ]
    lines += board_s3_footprint()
    lines += board_esp01_footprint()

    # Compact rectangular board: 24 x 33 mm.
    outline = [
        ((BOARD_LEFT, BOARD_TOP), (BOARD_RIGHT, BOARD_TOP)),
        ((BOARD_RIGHT, BOARD_TOP), (BOARD_RIGHT, BOARD_BOTTOM)),
        ((BOARD_RIGHT, BOARD_BOTTOM), (BOARD_LEFT, BOARD_BOTTOM)),
        ((BOARD_LEFT, BOARD_BOTTOM), (BOARD_LEFT, BOARD_TOP)),
    ]
    for index, (start, end) in enumerate(outline, start=1):
        lines += gr_line(index, start, end)

    lines += gr_text(1, "ESP32-S3-ZERO", 100, 87.7, 0.8)
    lines += gr_text(2, "ESP-01", 100, 106.5, 0.8)
    lines += gr_text(3, "BOTTOM", 100, 102.0, 0.8, "B.SilkS", mirrored=True)
    lines += gr_text(4, "TOP", 100, 102.0, 0.8)

    # All routes are generated from the checked model above.  Only the
    # local +3V3 segment between U1.9 and J1.4 is non-orthogonal.
    for net in ("UART_TX", "GND", "UART_RX", "+3V3"):
        lines += route_segments(net)

    lines += [
        "\t(embedded_fonts no)",
        ")",
    ]
    write_generated(BOARD, "\n".join(lines) + "\n")


def generate_project_file() -> None:
    default_class = {
        "bus_width": 12,
        "clearance": 0.25,
        "diff_pair_gap": 0.25,
        "diff_pair_via_gap": 0.25,
        "diff_pair_width": 0.25,
        "line_style": 0,
        "microvia_diameter": 0.3,
        "microvia_drill": 0.1,
        "name": "Default",
        "pcb_color": "rgba(0, 0, 0, 0.000)",
        "priority": 2147483647,
        "schematic_color": "rgba(0, 0, 0, 0.000)",
        "track_width": 0.35,
        "tuning_profile": "",
        "via_diameter": 0.8,
        "via_drill": 0.4,
        "wire_width": 6,
    }
    power_class = dict(default_class)
    power_class.update({"name": "Power", "priority": 0, "track_width": 1.0})
    data = {
        "board": {},
        "boards": [],
        "cvpcb": {"equivalence_files": []},
        "erc": {"erc_exclusions": [], "meta": {"version": 0}, "pin_map": [], "rule_severities": {}},
        "libraries": {"pinned_footprint_libs": [], "pinned_symbol_libs": []},
        "meta": {"filename": PROJECT_FILE.name, "version": 1},
        "net_settings": {
            "classes": [default_class, power_class],
            "meta": {"version": 5},
            "net_colors": None,
            "netclass_assignments": None,
            "netclass_patterns": [
                {"netclass": "Power", "pattern": "+3V3"},
                {"netclass": "Power", "pattern": "GND"},
            ],
        },
        "pcbnew": {},
        "schematic": {},
        "sheets": [[uid(f"{PROJECT}.kicad_sch:1:sheet"), ""]],
        "text_variables": {},
    }
    write_generated(PROJECT_FILE, json.dumps(data, indent=2, ensure_ascii=False) + "\n")


def generate_library_tables() -> None:
    sym_table = """(sym_lib_table
  (lib (name "ZifiAdapter")(type "KiCad")
    (uri "${KIPRJMOD}/Libraries/Symbols/ZifiAdapter.kicad_sym")
    (options "")(descr "ZiFi ESP32-S3-Zero adapter symbols"))
)
"""
    fp_table = """(fp_lib_table
  (lib (name "ZifiAdapter")(type "KiCad")
    (uri "${KIPRJMOD}/Libraries/Footprints/ZifiAdapter.pretty")
    (options "")(descr "ZiFi ESP32-S3-Zero adapter footprints"))
)
"""
    write_generated(ROOT / "sym-lib-table", sym_table)
    write_generated(ROOT / "fp-lib-table", fp_table)


def validate_contract() -> None:
    s3_symbol = {pin[0] for pin in S3_PINS}
    s3_footprint = {str(n) for n in range(1, 19)}
    esp_symbol = {pin[0] for pin in ESP01_PINS}
    esp_footprint = {str(n) for n in range(1, 9)}
    assert s3_symbol == s3_footprint
    assert esp_symbol == esp_footprint
    assert set(S3_USED.values()) == {"GND", "+3V3", "UART_TX", "UART_RX"}
    assert set(ESP01_USED.values()) == {"GND", "+3V3", "UART_TX", "UART_RX"}
    assert REF_DIR.joinpath("ESP32-S3-Zero.step").is_file(), "official STEP model is missing"

    left_hole_x = U1_X + S3_LEFT_ROW_X
    right_hole_x = U1_X + S3_RIGHT_ROW_X
    left_copper_x = left_hole_x - S3_PAD_OUTWARD_OFFSET
    right_copper_x = right_hole_x + S3_PAD_OUTWARD_OFFSET
    assert abs((left_copper_x + right_copper_x) / 2 - U1_X) < 1e-9
    left_outer_edge = left_copper_x - S3_PLANAR_PAD_SIZE[0] / 2
    right_outer_edge = right_copper_x + S3_PLANAR_PAD_SIZE[0] / 2
    assert abs((left_outer_edge - BOARD_LEFT) - (BOARD_RIGHT - right_outer_edge)) < 1e-9
    assert abs(left_outer_edge - 88.58) < 1e-9
    assert abs(right_outer_edge - 111.42) < 1e-9

    diagonal_segments: list[tuple[str, int]] = []
    for net, points in ROUTES.items():
        directions = []
        for index, (start, end) in enumerate(zip(points, points[1:])):
            dx = end[0] - start[0]
            dy = end[1] - start[1]
            assert dx != 0 or dy != 0, f"{net}: zero-length segment"
            if dx > 0 and dy == 0:
                directions.append("right")
            elif dx < 0 and dy == 0:
                directions.append("left")
            elif dx == 0 and dy > 0:
                directions.append("down")
            elif dx == 0 and dy < 0:
                directions.append("up")
            else:
                assert abs(abs(dx) - abs(dy)) < 1e-9, (
                    f"{net}: non-orthogonal segment is not 45 degrees"
                )
                diagonal_segments.append((net, index))
                horizontal = "right" if dx > 0 else "left"
                vertical = "down" if dy > 0 else "up"
                directions.append(f"{vertical}-{horizontal}-45")
        assert tuple(directions) == ROUTE_DIRECTIONS[net], (
            f"{net}: expected {ROUTE_DIRECTIONS[net]}, got {tuple(directions)}"
        )
    assert diagonal_segments == [("+3V3", 2)], (
        f"expected one local +3V3 diagonal, got {diagonal_segments}"
    )

    diagonal_c = POWER_DIAGONAL_START[0] - POWER_DIAGONAL_START[1]
    assert abs(diagonal_c - POWER_DIAGONAL_X_MINUS_Y) < 1e-9
    pin4_clearance = (
        (diagonal_c - (ESP01_PIN4[0] - ESP01_PIN4[1])) / math.sqrt(2)
        - PAD_END_RADIUS
        - POWER_TRACE_HALF_WIDTH
    )
    pad9_clearance = (
        (
            (
                ESP32_PAD9_COPPER_CENTER[0]
                - ESP32_PAD9_COPPER_CENTER[1]
            )
            - diagonal_c
        ) / math.sqrt(2)
        - PAD_END_RADIUS
        - ESP32_PAD_HALF_STRAIGHT / math.sqrt(2)
        - POWER_TRACE_HALF_WIDTH
    )
    assert abs(pin4_clearance - pad9_clearance) < 1e-9
    assert pin4_clearance > 0.25


def main() -> None:
    validate_contract()
    generate_symbol_library()
    generate_s3_footprint()
    generate_esp01_footprint()
    generate_schematic()
    generate_board()
    generate_project_file()
    generate_library_tables()
    print("generation complete")


if __name__ == "__main__":
    main()
