"""Convert the Classic client's volumetric fog tables into data/fogdata.bin.

Input: the Light, LightData and LightDataGlobalVolumeFog CSV exports (evidence/modern-extract/db2csv
in coa-vfog-kit.zip, or a directory holding them). Output layout, little-endian, matches src/fog_data.cpp:

  header  'VFD1', u32 version, u32 lights, u32 params, u32 keys, u32 layers
  light   u32 id, i32 map, f32 x, y, z, f32 falloffStart, falloffEnd, u32 params[8]
  params  u32 id, u32 firstKey, u32 keyCount                     (sorted by id)
  key     u16 time (half-minutes), u16 layerCount, u32 firstLayer (sorted by time)
  layer   u32 diffuse, emissive, shadowEmissive, flags,
          f32 start, density, shadowMultiplier, upperDensity, upperHeight,
              lowerDensity, lowerHeight, intensity, g, strength, exponent

Rows follow the Classic client's selection: flag bit 3 set, ordered by layer index, at most 9 per key.
Keys without selected rows are dropped, matching the client's fallback to the neighbouring key.
"""

import argparse
import csv
import io
import os
import struct
import sys
import zipfile

KIT_PREFIX = "coa-vfog-kit/evidence/modern-extract/db2csv/"
MAX_LAYERS_PER_KEY = 9
SELECTED_FLAG = 0x8
SLOTS = 8
VERSION = 1


def field(row, index):
    return row["Field_1_60_1_69876_%03d" % index]


def as_float(text):
    return float(text) if text else 0.0


def as_color(text):
    return int(float(text)) & 0xFFFFFF if text else 0


def read_tables(source):
    names = ("Light.csv", "LightData.csv", "LightDataGlobalVolumeFog.csv")
    tables = {}
    if zipfile.is_zipfile(source):
        with zipfile.ZipFile(source) as archive:
            for name in names:
                tables[name] = archive.read(KIT_PREFIX + name).decode("utf-8")
    else:
        for name in names:
            with open(os.path.join(source, name), encoding="utf-8") as handle:
                tables[name] = handle.read()
    return {name: list(csv.DictReader(io.StringIO(text))) for name, text in tables.items()}


def select_layers(rows):
    selected = [r for r in rows if int(float(field(r, 22) or 0)) & SELECTED_FLAG]
    selected.sort(key=lambda r: (int(float(field(r, 23) or 0)), int(r["ID"])))
    return selected[:MAX_LAYERS_PER_KEY]


def pack_layer(row):
    return struct.pack(
        "<4I11f",
        as_color(field(row, 1)),
        as_color(field(row, 2)),
        as_color(field(row, 3)),
        int(float(field(row, 22) or 0)) & 0xFFFFFFFF,
        as_float(field(row, 5)),
        as_float(field(row, 6)),
        as_float(field(row, 7)),
        as_float(field(row, 8)),
        as_float(field(row, 10)),
        as_float(field(row, 11)),
        as_float(field(row, 12)),
        as_float(field(row, 14)),
        as_float(field(row, 15)),
        as_float(field(row, 25)),
        as_float(field(row, 26)),
    )


def convert(tables):
    fog_by_data = {}
    for row in tables["LightDataGlobalVolumeFog.csv"]:
        fog_by_data.setdefault(row["LightDataID"], []).append(row)

    keys_by_params = {}
    for row in tables["LightData.csv"]:
        layers = select_layers(fog_by_data.get(row["ID"], []))
        if layers:
            time = int(float(row["Time"])) & 0xFFFF
            keys_by_params.setdefault(int(row["LightParamID"]), []).append((time, layers))

    lights = []
    used_params = set()
    for row in tables["Light.csv"]:
        params = [int(float(row["LightParamsID_%d" % i] or 0)) for i in range(SLOTS)]
        used_params.update(p for p in params if p in keys_by_params)
        lights.append(
            struct.pack(
                "<Ii5f8I",
                int(row["ID"]),
                int(float(row["ContinentID"])),
                as_float(row["GameCoords_0"]),
                as_float(row["GameCoords_1"]),
                as_float(row["GameCoords_2"]),
                as_float(row["GameFalloffStart"]),
                as_float(row["GameFalloffEnd"]),
                *params,
            )
        )

    params_blob, keys_blob, layers_blob = [], [], []
    key_count = layer_count = 0
    for params_id in sorted(used_params):
        keys = sorted(keys_by_params[params_id], key=lambda k: k[0])
        params_blob.append(struct.pack("<3I", params_id, key_count, len(keys)))
        for time, layers in keys:
            keys_blob.append(struct.pack("<2HI", time, len(layers), layer_count))
            layers_blob.extend(pack_layer(r) for r in layers)
            layer_count += len(layers)
            key_count += 1

    header = struct.pack("<4s5I", b"VFD1", VERSION, len(lights), len(params_blob), key_count, layer_count)
    blob = header + b"".join(lights) + b"".join(params_blob) + b"".join(keys_blob) + b"".join(layers_blob)
    return blob, len(lights), len(params_blob), key_count, layer_count


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("source", help="coa-vfog-kit.zip or a directory with the CSV exports")
    parser.add_argument("output", help="output path, normally data/fogdata.bin")
    args = parser.parse_args()
    blob, lights, params, keys, layers = convert(read_tables(args.source))
    with open(args.output, "wb") as handle:
        handle.write(blob)
    print("%s: %d lights, %d light params, %d keys, %d layers, %d bytes"
          % (args.output, lights, params, keys, layers, len(blob)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
