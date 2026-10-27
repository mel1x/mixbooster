// The switches' work, off the UI thread. A switch flips on the click and posts a job; one worker thread runs the jobs
// in the order they came (two clicks on one switch apply in order), and each job's done() runs back on the UI thread
// with whether it worked, to roll the switch back and say so when it didn't.

typedef struct Job Job;
struct Job {
    bool (*run)(Job *j);            // on the worker thread; true when the change is in place
    void (*done)(Job *j, bool ok);  // on the UI thread
    int idx, seq;                   // the switch, and which click on it this was
    bool on;
    union {
        Game game;
        struct { GUID plan; bool dc; } park;
    } u;
    bool ok;
    Job *next;
};

static struct {
    CRITICAL_SECTION lock;
    HANDLE kick, thread;
    Job *head, *tail;
    volatile LONG busy;             // jobs posted and not done yet
    HWND notify;
} JQ;

static DWORD WINAPI job_thread(void *arg)
{
    (void)arg;
    for (;;) {
        WaitForSingleObject(JQ.kick, INFINITE);
        for (;;) {
            EnterCriticalSection(&JQ.lock);
            Job *j = JQ.head;
            if (j) {
                JQ.head = j->next;
                if (!JQ.head) JQ.tail = NULL;
            }
            LeaveCriticalSection(&JQ.lock);
            if (!j) break;
            j->ok = j->run(j);
            if (!PostMessageW(JQ.notify, WM_APP_JOB_DONE, 0, (LPARAM)j)) {
                free(j);
                InterlockedDecrement(&JQ.busy);
            }
        }
    }
}

static void job_post(HWND notify, const Job *proto)
{
    if (!JQ.kick) {
        InitializeCriticalSection(&JQ.lock);
        JQ.kick = CreateEventW(NULL, FALSE, FALSE, NULL);
        JQ.thread = CreateThread(NULL, 0, job_thread, NULL, 0, NULL);
    }
    Job *j = (Job *)malloc(sizeof *j);
    if (!j) return;
    *j = *proto;
    j->next = NULL;
    JQ.notify = notify;
    InterlockedIncrement(&JQ.busy);
    EnterCriticalSection(&JQ.lock);
    if (JQ.tail) JQ.tail->next = j;
    else JQ.head = j;
    JQ.tail = j;
    LeaveCriticalSection(&JQ.lock);
    SetEvent(JQ.kick);
}

// WM_APP_JOB_DONE.
static void job_finish(Job *j)
{
    InterlockedDecrement(&JQ.busy);
    if (!j->ok) log_msg("job for switch %d failed", j->idx);
    if (j->done) j->done(j, j->ok);
    free(j);
}

// Before the window goes: the jobs posted still run to the end, so no change is left half made.
static void job_drain(DWORD ms)
{
    for (DWORD t = 0; JQ.busy && t < ms; t += 20) {
        MSG m;
        while (PeekMessageW(&m, NULL, WM_APP_JOB_DONE, WM_APP_JOB_DONE, PM_REMOVE)) job_finish((Job *)m.lParam);
        Sleep(20);
    }
}
