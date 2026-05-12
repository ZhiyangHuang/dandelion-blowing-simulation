Project Guidelines
Overview
The term project requires you to design and implement a non-trivial operating systems component that demonstrates mastery of core OS mechanisms. Your project must go beyond simple application-level programming — it should engage deeply with kernel-level concepts such as synchronization primitives, thread coordination, memory management, resource scheduling, or inter-process communication.

Scope & Requirements
OS-level focus: The project must center on one or more core OS mechanisms — synchronization, scheduling, memory management, IPC, file systems, or device management. Simply using threads or system calls is not sufficient.
Implementation depth: You are expected to implement the mechanism yourself (e.g., a custom scheduler, allocator, or synchronization construct) rather than wrapping existing libraries.
Correctness & analysis: Include test cases demonstrating correctness, and a write-up analyzing design trade-offs, limitations, and performance characteristics.
Language: C or C++ strongly preferred. Python is acceptable for simulation-focused projects with instructor approval.
Project Ideas
User-space thread library with cooperative/preemptive scheduling
Memory allocator with compaction and fragmentation analysis
Page replacement simulator with working-set tracking
Readers-writers lock with priority policies
Simple log-structured file system
Deadlock detector for a simulated multi-resource system
Mini shell with job control (foreground/background, signals)
Producer-consumer pipeline with custom bounded buffer
You are encouraged to propose your own idea. Discuss with the instructor early.

Timeline
Milestone	Due
Progress check-in	Week 11 (April 8)
Grading
Component	Weight
Proposal	10%
Implementation	50%
Report	40%