// ── counter ───────────────────────────────────
let count = 0;
document.getElementById('btn').addEventListener('click', () => {
    count++;
    document.getElementById('count').textContent = count;
});

// ── fetch from API ────────────────────────────
document.getElementById('fetch-btn').addEventListener('click', async () => {
    const pre = document.getElementById('response');
    pre.textContent = 'loading...';
    try {
        const res  = await fetch('/api/hello');
        const data = await res.json();
        pre.textContent = JSON.stringify(data, null, 2);
    } catch (e) {
        pre.textContent = 'error: ' + e.message;
    }
});