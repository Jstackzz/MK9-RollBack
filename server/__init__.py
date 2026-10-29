"""MK9 Online lobby server (Mortal Kombat Komplete Edition): accounts, matchmaking, lobbies, leaderboards and UDP rendezvous.

One asyncio process, standard library only (sqlite3, ssl, hashlib). The wire protocol is
specified in docs/PROTOCOL.md.
"""

__version__ = "0.3.0"  # 0.3.0: connections and hidden addresses, lobby countdowns and seats, desync reports
