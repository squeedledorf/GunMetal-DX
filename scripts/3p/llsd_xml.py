"""Minimal LLSD-XML reader (standard library only).

Covers the subset used by the autobuild-package.xml metadata inside
3p archives: map/key, array, string/uri/date, integer, real, boolean, undef.
"""
import xml.etree.ElementTree as ET


def _value(node):
    tag = node.tag
    if tag == 'map':
        out, key = {}, None
        for child in node:
            if child.tag == 'key':
                key = child.text or ''
            else:
                out[key] = _value(child)
        return out
    if tag == 'array':
        return [_value(child) for child in node]
    if tag in ('string', 'uri', 'date'):
        return node.text or ''
    if tag == 'integer':
        return int(node.text or 0)
    if tag == 'real':
        return float(node.text or 0)
    if tag == 'boolean':
        return (node.text or '').strip().lower() in ('1', 'true')
    if tag == 'undef':
        return None
    raise ValueError('unsupported LLSD element <%s>' % tag)


def parse(path_or_file):
    root = ET.parse(path_or_file).getroot()
    if root.tag != 'llsd' or len(root) != 1:
        raise ValueError('%s is not an LLSD XML document' % path_or_file)
    return _value(root[0])
