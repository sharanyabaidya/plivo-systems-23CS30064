# NOTES.md — Real-Time UDP streaming design notes

1. We implemented an adaptive **Sliding-Window XOR FEC** protocol with prioritized retransmissions (ARQ).
2. The sender uses a priority-based bandwidth ceiling clamp to prioritize NACK retransmissions (up to 1.98× overhead) over proactive duplicates (up to 1.90× overhead).
3. The receiver uses a delay-adaptive sliding window ($W=2$ for delays $\le 60\text{ms}$ and $W=4$ for larger delays) with an iterative back-substitution decoder.
4. We systematically swept NACK grace windows, expiry timers, and FEC window sizes to analyze parameter sensitivity.
5. Observations of block-boundary drop failures led us to replace rigid block FEC with adaptive-window sliding FEC for optimal recovery.
6. Playout delays we should grade at are **45 ms** for Profile A, **100 ms** for Profile B, and **120 ms** for Profile C.
7. These delays provide the optimal balance between recovery probability and playout latency under each profile's drop rate and jitter profile.
8. The system is broken if the playout delay is set below the physical limits of the channel round-trip time.
9. Specifically, delays below $45\text{ms}$ on Profile A, $100\text{ms}$ on Profile B, or $120\text{ms}$ on Profile C will cause NACK retransmissions to arrive past their deadlines.
10. Additionally, uniform packet loss rates exceeding $22\%$ will breach the 2.0× bandwidth budget or exceed the 1.0% miss rate cap.
