# Copyright (c) 2026-present, the Ladyflock developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import re
import sys

from ipaddress import IPv6Address
from unicodedata import normalize
from urllib.parse import urlsplit


def parse_ipv4_number(part):
    if part.startswith("0x"):
        return int(part[2:] or "0", 16)
    return int(part, 8 if part.startswith("0") else 10)


def validate_domain_name(host):
    labels = host.split(".")
    # A host with a decimal or hexadecimal final label is parsed as an IPv4 address.
    if re.fullmatch(r"(?:[0-9]+|0x[0-9a-f]*)", labels[-1]):
        numbers = [parse_ipv4_number(label) for label in labels]
        if len(numbers) > 4 or any(number > 255 for number in numbers[:-1]) or numbers[-1] >= 256 ** (5 - len(numbers)):
            raise ValueError("Use a valid IPv4 address.")
        return

    for label in labels:
        if label.startswith("xn--"):
            # Decode Punycode directly so that IDNA 2003's transitional mappings do not reject names such as faß.
            decoded = label[4:].encode("ascii").decode("punycode")
            if (
                decoded.isascii()
                or normalize("NFKC", decoded) != decoded
                or any(not character.isprintable() and character not in "\u200c\u200d" for character in decoded)
            ):
                raise ValueError("Use a valid Punycode hostname.")


def validate_release_feed_url(url):
    if not re.fullmatch(r"[-A-Za-z0-9._~:/?#\[\]@!$&'()*+,;=%]+", url) or re.search(r"%(?![0-9a-fA-F]{2})", url):
        raise ValueError("Use URL-safe characters and valid percent escapes.")

    parts = urlsplit(url)
    host = parts.hostname
    if parts.scheme not in {"http", "https"} or not host:
        raise ValueError("Use an absolute HTTP or HTTPS URL with a host.")

    label = r"[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?"
    dns_name = rf"{label}(?:\.{label})*\.?"
    authority = parts.netloc.rsplit("@", 1)[-1]
    if not re.fullmatch(rf"(?:{dns_name}|\[[a-f0-9:.]+\])(?::[0-9]+)?", authority.lower()):
        raise ValueError("Use a valid hostname or IP address and optional port.")
    # Accessing the port checks its range.
    _ = parts.port

    host = host.removesuffix(".")
    if host == "ladybird.org" or host.endswith(".ladybird.org"):
        raise ValueError("The update feed must not point to ladybird.org. Configure a Ladyflock-owned feed URL.")
    if authority.startswith("["):
        IPv6Address(host)
    else:
        validate_domain_name(host)


if __name__ == "__main__":
    try:
        validate_release_feed_url(sys.argv[1])
    except ValueError as error:
        sys.exit(str(error))
