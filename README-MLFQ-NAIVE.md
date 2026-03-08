# XV6 Multi-Level Feedback Queue - Naive Implementation

This branch implements a **5-queue MLFQ scheduler** for the xv6-riscv operating system.

## Scheduler Specifications
- **Number of queues:** 5($Q_0$ to $Q_4$)
- **Time slices (Quanta):**
 -$Q_0$: 1 tick
 -$Q_1$: 2 ticks
 -$Q_2$: 4 ticks
 -$Q_3$: 8 ticks
 -$Q_4$: Round-Robin (16 ticks)
- **Priority boost:** every 256 ticks all processes are moved to $Q_0$ to prevent starvation.
- **Preemption:** High-priority processes preempt lower-priority ones on every timer interupt
- **Demotion policy:** Ticks are cumulative. If a process exhausts its entire time slice at its current level, it is demoted to the next lower priority queue.
- **I/O rewards:** Processes that sleep for I/O before exhausting their slice retain the current priority level.