# NOTES.md — Real-Time UDP streaming design notes

1. We implemented an adaptive **Sliding-Window XOR FEC** protocol with prioritized retransmissions (ARQ).
2. The sender uses a priority-based bandwidth ceiling clamp to prioritize NACK retransmissions (up to 1.98× overhead) over proactive duplicates (up to 1.90× overhead).
3. The receiver uses a delay-adaptive sliding window ($W=2$ for delays $\le 60\text{ms}$ and $W=4$ for larger delays) with an iterative back-substitution decoder.
4. Playout delays we should grade at are **45 ms** for Profile A, **100 ms** for Profile B, and **120 ms** for Profile C.
5. These delays provide the optimal balance between recovery probability and playout latency under each profile's drop rate and jitter profile.
6. The system is broken if the playout delay is set below the physical limits of the channel round-trip time.
7. Specifically, delays below $45\text{ms}$ on Profile A, $100\text{ms}$ on Profile B, or $120\text{ms}$ on Profile C will cause NACK retransmissions to arrive past their deadlines.
8. Additionally, uniform packet loss rates exceeding $22\%$ will breach the 2.0× bandwidth budget or exceed the 1.0% miss rate cap.
