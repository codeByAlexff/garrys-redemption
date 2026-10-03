"""The bridge link for the Python tools: shared memory, handshake and seqlock.

This mirrors protocol/gr_link.h and protocol/gr_seqlock.h in behaviour. The layout itself
is not mirrored: it comes from gr_layout.py, which reads protocol/gr_protocol.h.
tools/tests/ runs these against gr_probe, which is the real C++ code, to keep the two
from drifting apart.

Python cannot do real atomic stores. The seqlock counter and heartbeat are written with a
single aligned 4-byte copy, which on x86-64 is as good, and the fakes are test stand-ins,
not shipped code.
"""

from __future__ import annotations

import ctypes
import os
import struct
import time
from ctypes import wintypes

from gr_layout import Layout

DEFAULT_NAME = "Local\\GarrysRedemptionBridge"

_PAGE_READWRITE = 0x04
_FILE_MAP_READ = 0x0004
_FILE_MAP_ALL_ACCESS = 0x000F001F
_ERROR_ALREADY_EXISTS = 183
_INVALID_HANDLE_VALUE = wintypes.HANDLE(-1)

_k32 = ctypes.WinDLL("kernel32", use_last_error=True)
_k32.CreateFileMappingW.restype = wintypes.HANDLE
_k32.CreateFileMappingW.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD,
                                    wintypes.DWORD, wintypes.DWORD, wintypes.LPCWSTR]
_k32.OpenFileMappingW.restype = wintypes.HANDLE
_k32.OpenFileMappingW.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.LPCWSTR]
_k32.MapViewOfFile.restype = ctypes.c_void_p
_k32.MapViewOfFile.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.DWORD, wintypes.DWORD,
                               ctypes.c_size_t]
_k32.UnmapViewOfFile.argtypes = [ctypes.c_void_p]
_k32.CloseHandle.argtypes = [wintypes.HANDLE]


class _MEMORY_BASIC_INFORMATION(ctypes.Structure):
    _fields_ = [("BaseAddress", ctypes.c_void_p), ("AllocationBase", ctypes.c_void_p),
                ("AllocationProtect", wintypes.DWORD), ("PartitionId", wintypes.WORD),
                ("RegionSize", ctypes.c_size_t), ("State", wintypes.DWORD),
                ("Protect", wintypes.DWORD), ("Type", wintypes.DWORD)]


_k32.VirtualQuery.restype = ctypes.c_size_t
_k32.VirtualQuery.argtypes = [ctypes.c_void_p, ctypes.POINTER(_MEMORY_BASIC_INFORMATION),
                              ctypes.c_size_t]


def now_ms() -> int:
    return time.monotonic_ns() // 1_000_000


def now_us() -> int:
    """gr::NowUs() in protocol/gr_link.h: microseconds on the performance counter, which
    every process on the machine shares, so a frame's time_us can be compared with ours.
    perf_counter is that counter on Windows. Wraps like the C++ uint32_t."""
    return (time.perf_counter_ns() // 1000) & 0xFFFFFFFF


def joaat(text: str) -> int:
    """Jenkins one-at-a-time hash of the lowercased string, as RDR2's GET_HASH_KEY does it."""
    h = 0
    for ch in text.lower().encode("utf-8"):
        h = (h + ch) & 0xFFFFFFFF
        h = (h + (h << 10)) & 0xFFFFFFFF
        h ^= h >> 6
    h = (h + (h << 3)) & 0xFFFFFFFF
    h ^= h >> 11
    h = (h + (h << 15)) & 0xFFFFFFFF
    return h


class Mapping:
    """A view of the named shared memory.

    create=True  creates it at `size` bytes if it does not exist, otherwise opens it.
    create=False opens an existing one read-only and maps all of it; raises
                 FileNotFoundError if neither side has created it.
    """

    def __init__(self, name: str, size: int = 0, create: bool = True):
        self.name = name
        self.created = False
        self._addr = None
        if create:
            self._handle = _k32.CreateFileMappingW(_INVALID_HANDLE_VALUE, None, _PAGE_READWRITE,
                                                   0, size, name)
            err = ctypes.get_last_error()
            if not self._handle:
                raise ctypes.WinError(err)
            self.created = err != _ERROR_ALREADY_EXISTS
            access = _FILE_MAP_ALL_ACCESS
        else:
            self._handle = _k32.OpenFileMappingW(_FILE_MAP_READ, False, name)
            if not self._handle:
                raise FileNotFoundError(name)
            access = _FILE_MAP_READ
            size = 0

        self._addr = _k32.MapViewOfFile(self._handle, access, 0, 0, size)
        if not self._addr:
            err = ctypes.get_last_error()
            _k32.CloseHandle(self._handle)
            self._handle = None
            # An existing mapping smaller than `size`: its creator has another layout.
            raise PermissionError(err, f"cannot map {size} bytes of {name}")

        if size == 0:
            info = _MEMORY_BASIC_INFORMATION()
            _k32.VirtualQuery(self._addr, ctypes.byref(info), ctypes.sizeof(info))
            size = info.RegionSize  # rounded up to whole pages
        self.size = size

    def close(self) -> None:
        if self._addr:
            _k32.UnmapViewOfFile(self._addr)
            self._addr = None
        if self._handle:
            _k32.CloseHandle(self._handle)
            self._handle = None

    def read(self, offset: int, size: int) -> bytes:
        if offset < 0 or offset + size > self.size:
            raise IndexError(f"read of {size} bytes at {offset} is outside the {self.size} byte view")
        return ctypes.string_at(self._addr + offset, size)

    def write(self, offset: int, data: bytes) -> None:
        if offset < 0 or offset + len(data) > self.size:
            raise IndexError(f"write of {len(data)} bytes at {offset} is outside the {self.size} byte view")
        ctypes.memmove(self._addr + offset, data, len(data))

    def u32(self, offset: int) -> int:
        return struct.unpack("<I", self.read(offset, 4))[0]

    def set_u32(self, offset: int, value: int) -> None:
        self.write(offset, struct.pack("<I", value & 0xFFFFFFFF))


def seq_read(mapping: Mapping, offset: int, size: int, tries: int = 8) -> bytes | None:
    """Seqlock read of the frame at `offset`. None if every try was torn."""
    for _ in range(tries):
        before = mapping.u32(offset)
        if before & 1:
            continue
        data = mapping.read(offset, size)
        if mapping.u32(offset) == before:
            return data
    return None


def seq_write(mapping: Mapping, offset: int, data: bytes) -> None:
    """Seqlock write. The first four bytes of `data` (the seq field) are ignored."""
    odd = mapping.u32(offset) | 1
    mapping.set_u32(offset, odd)
    mapping.write(offset + 4, data[4:])
    mapping.set_u32(offset, odd + 1)


class Link:
    """One side of the bridge. role is 'host' (RDR2) or 'guest' (GMod).

    protocol_version and shm_size can be overridden to play a mismatched build.
    """

    def __init__(self, role: str, name: str = DEFAULT_NAME, layout: Layout | None = None,
                 protocol_version: int | None = None):
        if role not in ("host", "guest"):
            raise ValueError("role must be 'host' or 'guest'")
        self.role = role
        self.name = name
        self.layout = layout or Layout()
        c = self.layout.const
        self.version = c("GR_PROTOCOL_VERSION") if protocol_version is None else protocol_version
        self.shm_size = c("GR_SHM_SIZE")
        self.timeout_ms = c("GR_PEER_TIMEOUT_MS")
        other = "guest" if role == "host" else "host"
        self._mine = self.layout.offset("GrShared", f"header.{role}")
        self._theirs = self.layout.offset("GrShared", f"header.{other}")
        self._my_frame = ("GrHostFrame", self.layout.offset("GrShared", "host")) if role == "host" \
            else ("GrGuestFrame", self.layout.offset("GrShared", "guest"))
        self._their_frame = ("GrGuestFrame", self.layout.offset("GrShared", "guest")) if role == "host" \
            else ("GrHostFrame", self.layout.offset("GrShared", "host"))
        self._peer_field = {f.name: f.offset for f in self.layout.structs["GrPeer"].fields}

        self.mapping: Mapping | None = None
        self.state = "closed"
        self.peer: dict = self.layout.unpack("GrPeer", bytes(self.layout.sizeof("GrPeer")))
        self.peer_generation = 0
        self.torn_reads = 0
        self._full = False
        self._mismatch = False
        self._refuse_reason = 0
        self._seen_change = False
        self._watched_epoch = 0
        self._watched_heartbeat = 0
        self._watched_change_ms = 0
        self._last_map_try_ms = 0

    # ---- lifecycle ----

    def open(self, now: int | None = None) -> None:
        self.close()
        self._map(now_ms() if now is None else now)

    def close(self) -> None:
        self._unmap()
        self.state = "closed"

    def refuse(self, reason: int) -> None:
        self._refuse_reason = reason
        if self.mapping:
            self._publish_own_state()

    def _map(self, now: int) -> None:
        self._last_map_try_ms = now
        try:
            self.mapping = Mapping(self.name, self.shm_size)
            self._full = True
        except PermissionError:
            self.mapping = Mapping(self.name, self.layout.const("GR_HEADER_SIZE"))
            self._full = False

        if self._full:
            struct_name, offset = self._my_frame
            seq_write(self.mapping, offset, bytes(self.layout.sizeof(struct_name)))

        m, f = self.mapping, self._peer_field
        m.set_u32(self._mine + f["magic"], 0)
        epoch = (time.perf_counter_ns() ^ (os.getpid() << 16)) & 0xFFFFFFFF
        if epoch == 0 or epoch == m.u32(self._mine + f["epoch"]):
            epoch = (epoch + 1) & 0xFFFFFFFF
        m.set_u32(self._mine + f["protocol_version"], self.version)
        m.set_u32(self._mine + f["shm_size"], self.shm_size)
        m.set_u32(self._mine + f["pid"], os.getpid())
        m.set_u32(self._mine + f["epoch"], epoch)
        self._mismatch = False
        self._publish_own_state()
        m.set_u32(self._mine + f["magic"], self.layout.const("GR_MAGIC"))

        self.peer = self._snapshot_peer()
        self._watched_epoch = self.peer["epoch"]
        self._watched_heartbeat = self.peer["heartbeat"]
        self._watched_change_ms = now
        self._seen_change = False
        self.state = "waiting"

    def _unmap(self) -> None:
        if self.mapping:
            self.mapping.set_u32(self._mine + self._peer_field["state"],
                                 self.layout.const("GR_PEER_GONE"))
            self.mapping.close()
            self.mapping = None
        self._full = False

    def _publish_own_state(self) -> None:
        c = self.layout.const
        state, reason = c("GR_PEER_ALIVE"), c("GR_REASON_NONE")
        if self._refuse_reason:
            state, reason = c("GR_PEER_REFUSED"), self._refuse_reason
        elif self._mismatch:
            state, reason = c("GR_PEER_REFUSED"), c("GR_REASON_VERSION")
        self.mapping.set_u32(self._mine + self._peer_field["reason"], reason)
        self.mapping.set_u32(self._mine + self._peer_field["state"], state)

    # ---- per frame ----

    def tick(self, now: int | None = None) -> str:
        now = now_ms() if now is None else now
        c = self.layout.const
        hb = self._mine + self._peer_field["heartbeat"]
        self.mapping.set_u32(hb, self.mapping.u32(hb) + 1)

        self.peer = self._snapshot_peer()
        alive = self._watch_peer(self.peer, now)

        if not self._full and not alive and now - self._last_map_try_ms >= 1000:
            self._unmap()
            self._map(now)

        mismatch = alive and (not self._full or self.peer["protocol_version"] != self.version
                              or self.peer["shm_size"] != self.shm_size)
        if mismatch != self._mismatch:
            self._mismatch = mismatch
            self._publish_own_state()

        if self._refuse_reason:
            self.state = "refusing"
        elif not alive:
            self.state = "waiting"
        elif mismatch:
            self.state = "version-mismatch"
        elif self.peer["state"] == c("GR_PEER_REFUSED"):
            self.state = "peer-refused"
        else:
            self.state = "connected"
        return self.state

    def _snapshot_peer(self) -> dict:
        size = self.layout.sizeof("GrPeer")
        for _ in range(4):
            a = self.mapping.read(self._theirs, size)
            b = self.mapping.read(self._theirs, size)
            pa, pb = self.layout.unpack("GrPeer", a), self.layout.unpack("GrPeer", b)
            if all(pa[k] == pb[k] for k in ("magic", "epoch", "protocol_version", "shm_size")):
                return pb
        return self.layout.unpack("GrPeer", bytes(size))

    def _watch_peer(self, p: dict, now: int) -> bool:
        c = self.layout.const
        if p["magic"] != c("GR_MAGIC") or p["state"] in (c("GR_PEER_ABSENT"), c("GR_PEER_GONE")):
            self._seen_change = False
            return False
        if p["epoch"] != self._watched_epoch:
            self.peer_generation += 1
            self._watched_epoch = p["epoch"]
            self._watched_heartbeat = p["heartbeat"]
            self._watched_change_ms = now
            self._seen_change = True
        elif p["heartbeat"] != self._watched_heartbeat:
            self._watched_heartbeat = p["heartbeat"]
            self._watched_change_ms = now
            self._seen_change = True
        if now - self._watched_change_ms > self.timeout_ms:
            self._seen_change = False
        return self._seen_change

    # ---- frames ----

    def publish(self, frame: dict) -> None:
        """Writes this side's frame (GrHostFrame for a host, GrGuestFrame for a guest)."""
        if not (self.mapping and self._full) or self._refuse_reason:
            return
        struct_name, offset = self._my_frame
        seq_write(self.mapping, offset, self.layout.pack(struct_name, frame))

    def read(self) -> dict | None:
        """The other side's frame, or None when not connected or the read was torn."""
        if self.state != "connected":
            return None
        struct_name, offset = self._their_frame
        data = seq_read(self.mapping, offset, self.layout.sizeof(struct_name))
        if data is None:
            self.torn_reads += 1
            return None
        return self.layout.unpack(struct_name, data)
