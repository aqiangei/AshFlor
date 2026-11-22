// Prepended to the Node server's glue (--pre-js, cpp/CMakeLists.txt). It runs
// before the module does, in the glue's own scope, which is how it reaches
// Module.
//
// What a crash leaves in the log. Left to Node's default handler, a wasm trap
// or an abort -- and in this build every C++ throw is an abort, because
// catching is linked out -- printed the source line it was thrown from. The
// glue is minified onto ONE line, so server-error.log got 96KB of JavaScript
// and a caret line of spaces, then ten frames of glue and not one C++ name.
// Here it gets a [crash] report: when, how long the server had been up, how
// big it was, the error, and its stack -- deep enough to get past the glue,
// and naming the C++ functions it runs through, which the release link keeps
// with --profiling-funcs.
(() => {
    const {isMainThread, threadId} = require("node:worker_threads");
    const fs = require("node:fs");

    // Set the stack limit in each worker as well. Emscripten forwards an
    // abort to the main thread after unwinding the original native stack.
    Error.stackTraceLimit = 64;
    if (!isMainThread) {
        Module["onAbort"] = reason => {
            const heapBytes = typeof wasmMemory === "undefined" ? 0 : wasmMemory.buffer.byteLength;
            const diagnostic = new Error(`Aborted(${reason})`);
            try {
                fs.writeSync(2, `[crash-worker] thread=${threadId} wasmHeapBytes=${heapBytes}\n` +
                    diagnostic.stack + "\n");
            } catch (_) {
            }
        };
        return;
    }

    // A start line not preceded by "shut down cleanly", "stopped for restart"
    // or a [crash] report means the last process was killed from outside --
    // SIGKILL, which on a box with no swap is most likely the kernel's OOM
    // killer -- and a process cannot report its own SIGKILL. This line is
    // what makes that gap readable in the log.
    console.log(`[server] starting at ${new Date().toISOString()}, pid ${process.pid}, ` +
                `Node ${process.version}`);

    const uptime = () => {
        const seconds = Math.floor(process.uptime());
        const h = Math.floor(seconds / 3600);
        const m = Math.floor(seconds / 60) % 60;
        return h > 0 ? `${h}h ${m}m` : m > 0 ? `${m}m ${seconds % 60}s` : `${seconds}s`;
    };

    let reported = false;
    let abortStack;
    // writeSync, not console.error: the process exits straight after this,
    // and Node writes to a pipe asynchronously on some platforms.
    const report = (what, error) => {
        if (reported) return;
        reported = true;
        let text = `[crash] ${new Date().toISOString()}: ${what} after ${uptime()} up, ` +
                   `${Math.round(process.memoryUsage.rss() / 1048576)} MB resident\n`;
        const stack = error && error.stack ? String(error.stack) : String(error);
        text += stack + "\n";
        if (typeof wasmMemory !== "undefined" && wasmMemory) {
            text += `[crash-memory] wasmHeapBytes=${wasmMemory.buffer.byteLength}\n`;
        }
        if (abortMessageContext) text += abortMessageContext;
        if (stack.includes("Aborted(")) {
            text += "[crash] an abort is a failed check or a C++ throw; the first C++ frame " +
                    "above, under abort and the glue, is where it came from\n";
        }
        try {
            fs.writeSync(2, text);
        } catch (e) {
        }
    };

    // An abort is final: the module refuses every call after it. Should
    // whatever called in catch the error, nothing else would end the process
    // and the server would sit there listening, never stepping again -- so
    // the exit is armed here too, a turn later, for an abort the handler
    // below never sees. The stack is taken now, while it still runs through
    // the C++ that aborted.
    let abortMessageContext = "";
    Module["onAbort"] = (reason) => {
        const address = Module["activeMessageContext"];
        if (Number.isInteger(address) && address > 0 && typeof HEAPU32 !== "undefined") {
            const index = address >>> 2;
            if (index + 2 < HEAPU32.length) {
                const kind = HEAPU32[index];
                if (kind > 0 && kind <= 256) {
                    abortMessageContext = "[crash-message] type=" + (kind - 1) +
                        " bytes=" + HEAPU32[index + 1] + " stage=" + HEAPU32[index + 2] + "\n";
                }
            }
        }
        abortStack = new Error(`Aborted(${reason})`);
        setTimeout(() => {
            report("the server aborted", abortStack);
            process.exit(1);
        }, 0);
    };

    // Covers unhandled rejections as well: Node raises those here when
    // nothing listens for them, which nothing does.
    process.on("uncaughtException", (error, origin) => {
        // The glue's exit(): a deliberate way out, not a crash.
        if (error && error.name === "ExitStatus") process.exit(error.status);
        report(origin === "unhandledRejection" ? "unhandled promise rejection"
                                               : "uncaught exception", abortStack || error);
        process.exit(1);
    });
})();
