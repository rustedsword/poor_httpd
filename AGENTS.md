1. No need to maintain backwards compatibility.
2. The current C standard is C23. Use all features of the language when needed.
3. Do not zero initialize objects. Provide initializer macro and init() function that will initialize only required fields.
4. Trust the user: do not check arguments when not neeeded. Use assert() / static_assert().
5. Do not run tests when asked for review. They were already ran before. Run tests only when you know that something changed (code, environemnt, etc...)
6. No synchronous operations: all I/O goes through the loop, logging and closing fds included. The only exceptions are calls io_uring cannot do, like sigprocmask() or signalfd(), and reporting that the loop itself failed.
7. All requests on an fd should be completed before its close can begin. poor_loop groups and fd groups (poor_loop_group.h, poor_loop_fd.h) were made for this case.
