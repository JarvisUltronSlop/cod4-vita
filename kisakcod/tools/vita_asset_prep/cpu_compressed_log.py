"""Read bounded, independently checksummed zlib members from Vita log sinks."""
import zlib


def lines(stream):
    decoder = None
    member = bytearray()
    pending = bytearray()
    while chunk := stream.read(65536):
        while chunk:
            if decoder is None:
                decoder = zlib.decompressobj()
            member.extend(decoder.decompress(chunk, 32769-len(member)))
            if len(member) > 32768:
                raise ValueError('compressed log member exceeds 32 KiB')
            chunk = decoder.unused_data if decoder.eof else decoder.unconsumed_tail
            if decoder.eof:
                # Do not publish any bytes until this member's checksum passed.
                pending.extend(member)
                member.clear()
                decoder = None
                while (at := pending.find(b'\n')) >= 0:
                    yield bytes(pending[:at+1])
                    del pending[:at+1]
                if len(pending) > 65536:
                    raise ValueError('compressed log record exceeds 64 KiB')
    if decoder is not None:
        raise ValueError('incomplete compressed log member')
    if pending:
        raise ValueError('incomplete compressed JSON record')
