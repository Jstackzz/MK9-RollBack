"""MK9 Online lobby server (Mortal Kombat Komplete Edition): accounts, matchmaking, lobbies, leaderboards and UDP rendezvous.

One asyncio process, standard library only (sqlite3, ssl, hashlib). The wire protocol is
specified in docs/PROTOCOL.md.
"""

# 0.3.0: connections and hidden addresses, lobby countdowns and seats, desync reports; 0.3.1: series rematches at the
# games' CONTINUE? menu through the relay, rollback's auto delay by MK3D Online's rule, a slow hidden match tries
# directly, ranked sets to three and rank titles; 0.3.2: crash notes; 0.3.3: F3 reports
__version__ = "0.3.3"
