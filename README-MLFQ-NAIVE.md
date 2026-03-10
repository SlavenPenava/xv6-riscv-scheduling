# XV6 Multi-Level Feedback Queue - Naive Implementation

This branch implements a **5-queue MLFQ scheduler** for the xv6-riscv operating system using a table-scan approach.

## Scheduler Specifications
- **Number of queues:** 5($Q_0$ to $Q_4$)
- **Time slices (Quanta):**
 - $Q_0$: 4 ticks
 - $Q_1$: 8 ticks
 - $Q_2$: 16 ticks
 - $Q_3$: 32 ticks
 - $Q_4$: 48 ticks
- **Priority boost:** every 256 ticks all processes are moved to $Q_0$ to prevent starvation.
- **Preemption:** High-priority processes strictly preempt lower-priority ones. The scheduler re-scans the table on every timer interrupt to ensure the highest priority RUNNABLE procces is on the CPU.
- **Demotion policy:** Ticks are cumulative at each level. If a process exhausts its entire assigned time slice at its current level, it is demoted to the next lower priority queue.
- **I/O rewards:** Processes that sleep for I/O before exhausting their slice retain the current priority level.

## How to run and log results
**1. Start xv6 with clean logging**
 - run this from your host terminal to clear the old log (or create new one if its first run) and capture all kernel output (including procdump triggered by ^T):
 > cat /dev/null > scheduler_log.txt && make qemu | tee scheduler_log.txt
**Run the test**
 - inside xv6 exists a custom "hog" script to simulate cpu bound and quicker processes where usage case is hog <cycles in millions>.
## Implementation details
This is a naive version of the MLFQ:
- 1. **Search complexity:** O(N*Queues) - The scheduler iterates through the process table for each priority level until a candidate is found.
- 2. **Timer interrupts:** The kernel cchecks ticks_consumed against the priority_qunta[p->priority] on every tick
- 3. **Synchronization:** Uses p->lock to ensure atomicity during priority shifts and state changes
