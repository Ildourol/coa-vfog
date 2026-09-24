import argparse
import csv
import io
import os
import struct
import sys
import zipfile

DESCRIPTION = "Convert the Classic client's volumetric fog tables into data/fogdata.bin."
KIT_PREFIX = "coa-vfog-kit/evidence/modern-extract/db2csv/"
TABLE_NAMES = ("Light.csv", "LightData.csv", "LightDataGlobalVolumeFog.csv")
MAX_LAYERS_PER_KEY = 9
CLIENT_SELECTED_FLAG = 0x8
LIGHT_PARAMS_SLOTS = 8

FILE_MAGIC = b"VFD1"
FORMAT_VERSION = 1
HEADER_FORMAT = "<4s5I"
LIGHT_FORMAT = "<Ii5f8I"
PARAMS_FORMAT = "<3I"
KEY_FORMAT = "<2HI"
LAYER_FORMAT = "<4I11f"
U16_MASK = 0xFFFF
U32_MASK = 0xFFFFFFFF

DIFFUSE_COLUMN = 1
EMISSIVE_COLUMN = 2
SHADOW_EMISSIVE_COLUMN = 3
START_COLUMN = 5
DENSITY_COLUMN = 6
SHADOW_MULTIPLIER_COLUMN = 7
UPPER_DENSITY_COLUMN = 8
UPPER_HEIGHT_COLUMN = 10
LOWER_DENSITY_COLUMN = 11
LOWER_HEIGHT_COLUMN = 12
INTENSITY_COLUMN = 14
G_COLUMN = 15
FLAGS_COLUMN = 22
LAYER_INDEX_COLUMN = 23
STRENGTH_COLUMN = 25
EXPONENT_COLUMN = 26


def field(row, index):
    return row["Field_1_60_1_69876_%03d" % index]


def as_float(text):
    return float(text) if text else 0.0


def as_int(text):
    return int(float(text or 0))


def as_color(text):
    return int(float(text)) & 0xFFFFFF if text else 0


def layer_flags(row):
    return as_int(field(row, FLAGS_COLUMN))


def layer_index(row):
    return as_int(field(row, LAYER_INDEX_COLUMN))


def read_tables(source):
    tables = {}
    if zipfile.is_zipfile(source):
        with zipfile.ZipFile(source) as archive:
            for name in TABLE_NAMES:
                tables[name] = archive.read(KIT_PREFIX + name).decode("utf-8")
    else:
        for name in TABLE_NAMES:
            with open(os.path.join(source, name), encoding="utf-8") as handle:
                tables[name] = handle.read()
    return {name: list(csv.DictReader(io.StringIO(text))) for name, text in tables.items()}


def client_selected_layers(rows):
    selected = [r for r in rows if layer_flags(r) & CLIENT_SELECTED_FLAG]
    selected.sort(key=lambda r: (layer_index(r), int(r["ID"])))
    return selected[:MAX_LAYERS_PER_KEY]


def pack_header(light_count, params_count, key_count, layer_count):
    return struct.pack(HEADER_FORMAT, FILE_MAGIC, FORMAT_VERSION, light_count, params_count, key_count, layer_count)


def pack_light(row, params_by_slot):
    return struct.pack(
        LIGHT_FORMAT,
        int(row["ID"]),
        int(float(row["ContinentID"])),
        as_float(row["GameCoords_0"]),
        as_float(row["GameCoords_1"]),
        as_float(row["GameCoords_2"]),
        as_float(row["GameFalloffStart"]),
        as_float(row["GameFalloffEnd"]),
        *params_by_slot,
    )


def pack_params(params_id, first_key, key_count):
    return struct.pack(PARAMS_FORMAT, params_id, first_key, key_count)


def pack_key(half_minute_of_day, layer_count, first_layer):
    return struct.pack(KEY_FORMAT, half_minute_of_day, layer_count, first_layer)


def pack_layer(row):
    return struct.pack(
        LAYER_FORMAT,
        as_color(field(row, DIFFUSE_COLUMN)),
        as_color(field(row, EMISSIVE_COLUMN)),
        as_color(field(row, SHADOW_EMISSIVE_COLUMN)),
        layer_flags(row) & U32_MASK,
        as_float(field(row, START_COLUMN)),
        as_float(field(row, DENSITY_COLUMN)),
        as_float(field(row, SHADOW_MULTIPLIER_COLUMN)),
        as_float(field(row, UPPER_DENSITY_COLUMN)),
        as_float(field(row, UPPER_HEIGHT_COLUMN)),
        as_float(field(row, LOWER_DENSITY_COLUMN)),
        as_float(field(row, LOWER_HEIGHT_COLUMN)),
        as_float(field(row, INTENSITY_COLUMN)),
        as_float(field(row, G_COLUMN)),
        as_float(field(row, STRENGTH_COLUMN)),
        as_float(field(row, EXPONENT_COLUMN)),
    )


def convert(tables):
    fog_by_data = {}
    for row in tables["LightDataGlobalVolumeFog.csv"]:
        fog_by_data.setdefault(row["LightDataID"], []).append(row)

    keys_by_params = {}
    for row in tables["LightData.csv"]:
        layers = client_selected_layers(fog_by_data.get(row["ID"], []))
        if layers:
            half_minute_of_day = int(float(row["Time"])) & U16_MASK
            keys_by_params.setdefault(int(row["LightParamID"]), []).append((half_minute_of_day, layers))

    lights = []
    used_params = set()
    for row in tables["Light.csv"]:
        params_by_slot = [as_int(row["LightParamsID_%d" % slot]) for slot in range(LIGHT_PARAMS_SLOTS)]
        used_params.update(p for p in params_by_slot if p in keys_by_params)
        lights.append(pack_light(row, params_by_slot))

    params_blob, keys_blob, layers_blob = [], [], []
    key_count = layer_count = 0
    for params_id in sorted(used_params):
        keys = sorted(keys_by_params[params_id], key=lambda k: k[0])
        params_blob.append(pack_params(params_id, key_count, len(keys)))
        for half_minute_of_day, layers in keys:
            keys_blob.append(pack_key(half_minute_of_day, len(layers), layer_count))
            layers_blob.extend(pack_layer(r) for r in layers)
            layer_count += len(layers)
            key_count += 1

    header = pack_header(len(lights), len(params_blob), key_count, layer_count)
    blob = header + b"".join(lights) + b"".join(params_blob) + b"".join(keys_blob) + b"".join(layers_blob)
    return blob, len(lights), len(params_blob), key_count, layer_count


def main():
    parser = argparse.ArgumentParser(description=DESCRIPTION)
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
