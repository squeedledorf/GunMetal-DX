#!/usr/bin/env python3
"""Regenerate the binary seed inputs under indra/fuzz/corpus/ (standard library only; the image
seeds use Pillow when it is installed, and are skipped with a note when it is not).

The checked-in seeds are the output of this script, so it only needs to run when a harness's
input layout changes.   python3 -I indra/fuzz/make_seeds.py

Input layouts that carry a leading selector byte (see each harness's header comment):
  fuzz_message        byte 0 bit 0 = circuit trusted, then the datagram
  fuzz_mesh_asset     byte 0 = LOD (0..4), then the asset
  fuzz_mesh_skin      byte 0 bit 0 = skin info (0) / decomposition (1), then zlib'd binary LLSD
  fuzz_gltf_material  byte 0 bit 0 = fromJSON (0) / LLSD override (1)
  fuzz_inventory      byte 0 bit 0 = item (0) / category (1)
"""
import os
import struct
import sys
import zlib

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "corpus")


def put(target, name, data):
    d = os.path.join(ROOT, target)
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, name), "wb") as f:
        f.write(data if isinstance(data, bytes) else data.encode())


# ---- binary LLSD encoder -------------------------------------------------------------------------
def llsd_bin(v):
    if v is None:
        return b"!"
    if v is True:
        return b"1"
    if v is False:
        return b"0"
    if isinstance(v, int):
        return b"i" + struct.pack(">i", v)
    if isinstance(v, float):
        return b"r" + struct.pack(">d", v)
    if isinstance(v, str):
        b = v.encode()
        return b"s" + struct.pack(">I", len(b)) + b
    if isinstance(v, bytes):
        return b"b" + struct.pack(">I", len(v)) + v
    if isinstance(v, list):
        return b"[" + struct.pack(">I", len(v)) + b"".join(llsd_bin(x) for x in v) + b"]"
    if isinstance(v, dict):
        out = b"{" + struct.pack(">I", len(v))
        for k, x in v.items():
            kb = k.encode()
            out += b"k" + struct.pack(">I", len(kb)) + kb + llsd_bin(x)
        return out + b"}"
    raise TypeError(type(v))


def zip_llsd(v):
    return zlib.compress(llsd_bin(v))


# ---- LLSD -----------------------------------------------------------------------------------------
llsd_map = {"name": "seed", "n": 7, "pi": 3.25, "ok": True, "none": None, "bin": b"\x01\x02\x03",
            "list": [1, 2.5, "three", [4], {"five": 5}]}
put("llsd_binary", "map.bin", llsd_bin(llsd_map))
put("llsd_binary", "scalar.bin", llsd_bin("hello"))
put("llsd_binary", "nested.bin", llsd_bin([[[[[[1]]]]], {"a": {"b": {"c": None}}}]))
put("llsd_auto", "binary_header.bin", b"<? llsd/binary ?>\n" + llsd_bin(llsd_map))
put("unzip_llsd", "map.zlib", zip_llsd(llsd_map))
put("unzip_llsd", "array.zlib", zip_llsd([{"a": 1}, {"b": [1, 2, 3]}]))

# ---- mesh -----------------------------------------------------------------------------------------
def u16s(*vals):
    return struct.pack("<%dH" % len(vals), *vals)


def face():
    # one triangle; positions / normals / texcoords are 16-bit quantised over the domain
    return {
        "PositionDomain": {"Min": [-0.5, -0.5, -0.5], "Max": [0.5, 0.5, 0.5]},
        "TexCoord0Domain": {"Min": [0.0, 0.0], "Max": [1.0, 1.0]},
        "Position": u16s(0, 0, 0, 65535, 0, 0, 0, 65535, 0),
        "Normal": u16s(32768, 32768, 65535, 32768, 32768, 65535, 32768, 32768, 65535),
        "TexCoord0": u16s(0, 0, 65535, 0, 0, 65535),
        "TriangleList": u16s(0, 1, 2),
    }


lod = [face(), face()]
lod_block = zip_llsd(lod)
put("mesh_lod", "two_faces.lod", lod_block)
put("mesh_lod", "no_geometry.lod", zip_llsd([{"NoGeometry": True}]))

none = {"offset": -1, "size": 0}
header = {"version": 1, "lowest_lod": none, "low_lod": none, "medium_lod": none,
          "high_lod": {"offset": 0, "size": len(lod_block)}, "physics_mesh": none, "skin": none,
          "physics_convex": none, "submodel_id": 0}
put("mesh_asset", "high_lod.bin", bytes([3]) + llsd_bin(header) + lod_block)

skin = {"joint_names": ["mPelvis", "mTorso"],
        "inverse_bind_matrix": [[1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0]] * 2,
        "bind_shape_matrix": [1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0],
        "pelvis_offset": 0.0}
put("mesh_skin", "skin.bin", b"\x00" + zip_llsd(skin))
decomp = {"HullList": bytes([4]), "Positions": u16s(0, 0, 0, 65535, 0, 0, 0, 65535, 0, 0, 0, 65535),
          "Min": [-0.5, -0.5, -0.5], "Max": [0.5, 0.5, 0.5],
          "BoundingVerts": u16s(0, 0, 0, 65535, 65535, 65535)}
put("mesh_skin", "decomposition.bin", b"\x01" + zip_llsd(decomp))


# ---- UDP messages (scripts/messages/message_template.msg) ------------------------------------------
def packet(flags, seq, number, body, zero=False):
    """number: ('H', n) high, ('M', n) medium, ('L', n) low frequency."""
    f, n = number
    num = {"H": bytes([n]), "M": bytes([255, n]), "L": bytes([255, 255]) + struct.pack(">H", n)}[f]
    payload = num + body
    if zero:
        flags |= 0x80
        out, i = b"", 0
        while i < len(payload):
            if payload[i] == 0:
                j = i
                while j < len(payload) and payload[j] == 0 and j - i < 255:
                    j += 1
                out += bytes([0, j - i])
                i = j
            else:
                out += bytes([payload[i]])
                i += 1
        payload = out
    return bytes([flags]) + struct.pack(">I", seq) + b"\x00" + payload


def var1(b):
    return bytes([len(b)]) + b


def var2(b):
    return struct.pack("<H", len(b)) + b


uuid = bytes(range(16))
put("message", "start_ping_check.bin",
    b"\x00" + packet(0x00, 1, ("H", 1), b"\x05" + struct.pack("<I", 0)))
put("message", "complete_ping_check.bin", b"\x00" + packet(0x40, 2, ("H", 2), b"\x05"))
put("message", "chat_from_viewer.bin",
    b"\x00" + packet(0x40, 3, ("L", 80), uuid + uuid + var2(b"hello\x00") + b"\x01" + struct.pack("<i", 0), zero=True))
quat = struct.pack("<3f", 0.0, 0.0, 0.0)
vec = struct.pack("<3f", 1.0, 0.0, 0.0)
put("message", "agent_update_zerocoded.bin",
    b"\x00" + packet(0x00, 4, ("H", 4),
                      uuid + uuid + quat + quat + b"\x00" + vec * 4 + struct.pack("<fIB", 256.0, 0, 0), zero=True))
put("message", "kill_object_trusted.bin",
    b"\x01" + packet(0x00, 5, ("H", 16), b"\x02" + struct.pack("<II", 100, 101)))
put("message", "layer_data_trusted.bin",
    b"\x01" + packet(0x00, 6, ("H", 11), b"\x4c" + var2(b"\x01\x02\x03\x04")))
put("message", "object_update_compressed_trusted.bin",
    b"\x01" + packet(0x00, 7, ("H", 13),
                      struct.pack("<QH", 1099511628032, 0) + b"\x01" + struct.pack("<I", 0) + var2(b"\x00" * 16)))
im = (uuid + uuid                                   # AgentData
      + b"\x00" + uuid + struct.pack("<I", 0) + uuid  # FromGroup, ToAgentID, ParentEstateID, RegionID
      + vec + b"\x00\x00" + uuid + struct.pack("<I", 0)  # Position, Offline, Dialog, ID, Timestamp
      + var1(b"Test Resident\x00") + var2(b"hi\x00") + var2(b"")
      + struct.pack("<I", 0)                        # EstateBlock
      + b"\x00")                                    # MetaData: zero blocks
put("message", "improved_instant_message.bin",
    b"\x00" + packet(0x40, 8, ("L", 254), im, zero=True))
# appended-acks form: flag 0x10, trailing ack ids and a count byte
put("message", "with_acks.bin",
    b"\x00" + packet(0x50, 9, ("H", 2), b"\x05") + struct.pack(">I", 1) + b"\x01")


# ---- glTF material --------------------------------------------------------------------------------
gltf = ('{"asset":{"version":"2.0"},"materials":[{"pbrMetallicRoughness":{"baseColorFactor":[1,0.5,0.25,1],'
        '"metallicFactor":0.5,"roughnessFactor":0.75},"emissiveFactor":[0,0,0],"alphaMode":"MASK",'
        '"alphaCutoff":0.5,"doubleSided":true,"extras":{"override_alpha_mode":true}}]}')
put("gltf_material", "material.json", b"\x00" + gltf.encode())
put("gltf_material", "override.llsd",
    b"\x01" + b"{'tex':[u11111111-2222-3333-4444-555555555555,u00000000-0000-0000-0000-000000000000],"
              b"'bc':[r1,r0.5,r0.25,r1],'ec':[r0,r0,r0],'mf':r0.5,'rf':r0.75,'ds':true,'am':i1,'ac':r0.5,"
              b"'ti':[{'o':[r0.5,r0.5],'s':[r2,r2],'r':r0.25},{},{},{}]}")

# ---- inventory / notecards ------------------------------------------------------------------------
ITEM = ("\t{\n\t\titem_id\t11111111-2222-3333-4444-555555555555\n"
        "\t\tparent_id\t00000000-0000-0000-0000-000000000000\n"
        "\tpermissions 0\n\t{\n\t\tbase_mask\t7fffffff\n\t\towner_mask\t7fffffff\n\t\tgroup_mask\t00000000\n"
        "\t\teveryone_mask\t00000000\n\t\tnext_owner_mask\t00082000\n"
        "\t\tcreator_id\t22222222-2222-3333-4444-555555555555\n"
        "\t\towner_id\t22222222-2222-3333-4444-555555555555\n"
        "\t\tlast_owner_id\t00000000-0000-0000-0000-000000000000\n"
        "\t\tgroup_id\t00000000-0000-0000-0000-000000000000\n\t}\n"
        "\t\tasset_id\t33333333-2222-3333-4444-555555555555\n\t\ttype\tnotecard\n\t\tinv_type\tnotecard\n"
        "\t\tflags\t00000000\n"
        "\tsale_info\t0\n\t{\n\t\tsale_type\tnot\n\t\tsale_price\t0\n\t}\n"
        "\t\tname\tSeed note|\n\t\tdesc\ta description|\n\t\tcreation_date\t1600000000\n\t}\n")
CATEGORY = ("\t{\n\t\tcat_id\t11111111-2222-3333-4444-555555555555\n"
            "\t\tparent_id\t00000000-0000-0000-0000-000000000000\n\t\ttype\tcategory\n\t\tpref_type\t-1\n"
            "\t\tname\tSeed folder|\n\t}\n")
put("inventory", "item.txt", b"\x00" + ITEM.encode())
put("inventory", "category.txt", b"\x01" + CATEGORY.encode())

put("notecard", "empty.note", "Linden text version 2\n{\nLLEmbeddedItems version 1\n{\ncount 0\n}\nText length 0\n}\n")
put("notecard", "text.note",
    "Linden text version 2\n{\nLLEmbeddedItems version 1\n{\ncount 0\n}\nText length 11\nhello world}\n")
put("notecard", "embedded_item.note",
    "Linden text version 2\n{\nLLEmbeddedItems version 1\n{\ncount 1\n{\next char index 0\ninv_item\t0\n"
    + ITEM + "}\n}\nText length 5\nhello}\n")


# ---- images (Pillow if present) ------------------------------------------------------------------
def tga_bytes():
    # 2x2, 24-bit uncompressed true-colour, bottom-left origin
    header = bytes([0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 2, 0, 24, 0])
    return header + bytes([255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255])


put("image_tga", "2x2_rgb.tga", tga_bytes())
put("image_bmp", "2x2_24bit.bmp",
    b"BM" + struct.pack("<IHHI", 70, 0, 0, 54)
    + struct.pack("<IiiHHIIiiII", 40, 2, 2, 1, 24, 0, 16, 2835, 2835, 0, 0)
    + bytes([255, 0, 0, 0, 255, 0, 0, 0]) + bytes([0, 0, 255, 255, 255, 255, 0, 0]))


def tiff_bytes():
    # baseline little-endian TIFF, 2x2 RGB, uncompressed, one strip
    pixels = bytes([255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255])
    entries = [
        (256, 3, 1, 2), (257, 3, 1, 2), (258, 3, 3, None), (259, 3, 1, 1), (262, 3, 1, 2),
        (273, 4, 1, None), (277, 3, 1, 3), (278, 3, 1, 2), (279, 4, 1, len(pixels)),
    ]
    ifd_off = 8
    ifd_len = 2 + len(entries) * 12 + 4
    bits_off = ifd_off + ifd_len
    data_off = bits_off + 6
    ifd = struct.pack("<H", len(entries))
    for tag, typ, cnt, val in entries:
        if tag == 258:
            val = bits_off
        elif tag == 273:
            val = data_off
        if typ == 4 or tag == 258:   # a LONG, or a value that is an offset: stored as 32 bits
            ifd += struct.pack("<HHII", tag, typ, cnt, val)
        else:                        # a lone SHORT: left-justified in the value field
            ifd += struct.pack("<HHIHH", tag, typ, cnt, val, 0)
    ifd += struct.pack("<I", 0)
    return b"II*\x00" + struct.pack("<I", ifd_off) + ifd + struct.pack("<3H", 8, 8, 8) + pixels


put("image_tiff", "2x2_rgb.tif", tiff_bytes())

try:
    import io
    from PIL import Image

    def pil(fmt, **kw):
        im = Image.new("RGB", (8, 8))
        im.putdata([(x * 30 % 256, y * 30 % 256, 128) for y in range(8) for x in range(8)])
        buf = io.BytesIO()
        im.save(buf, fmt, **kw)
        return buf.getvalue()

    put("image_png", "8x8_rgb.png", pil("PNG"))
    put("image_jpeg", "8x8_rgb.jpg", pil("JPEG", quality=75))
    put("image_tiff", "8x8_rgb_pil.tif", pil("TIFF", compression=None))
    try:
        put("image_j2c", "8x8_rgb.j2c", pil("JPEG2000", no_jp2=True))
    except Exception as e:  # PIL built without OpenJPEG
        print("note: no J2C seed (Pillow cannot write JPEG2000: %s)" % e, file=sys.stderr)
except ImportError:
    print("note: Pillow not installed; PNG / JPEG / J2C seeds not regenerated", file=sys.stderr)
