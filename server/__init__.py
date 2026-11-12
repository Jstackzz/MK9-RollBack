"""MK9 Online lobby server (Mortal Kombat Komplete Edition): accounts, matchmaking, lobbies, leaderboards and UDP rendezvous.

One asyncio process, standard library only (sqlite3, ssl, hashlib). The wire protocol is
specified in docs/PROTOCOL.md.
"""

# 0.3.0: connections and hidden addresses, lobby countdowns and seats, desync reports; 0.3.1: series rematches at the
# games' CONTINUE? menu through the relay, rollback's auto delay by MK3D Online's rule, a slow hidden match tries
# directly, ranked sets to three and rank titles; 0.3.2: crash notes; 0.3.3: F3 reports; 0.3.4: match stats (STATS lines,
# python -m server.matchlog); 0.3.5: rollback's auto input delay covers the one-way trip with half a frame to spare;
# 0.3.6: a lobby's games stay open for the same two's next match (welcome lobby_reuse, lobby_ready parked, reuse), the
# match stats take the screen's gaps, what made frames late and where the game stalled
__version__ = "0.3.6"
