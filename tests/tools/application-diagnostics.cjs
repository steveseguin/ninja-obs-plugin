// Passive, bounded error capture for native browser automation.
function installApplicationDiagnostics() {
  window.__applicationErrors = [];
  const describe = value => {
    try { return String(value && (value.stack || value)); }
    catch { return '[unprintable diagnostic value]'; }
  };
  const record = (kind, message) => {
    if (window.__applicationErrors.length < 200)
      window.__applicationErrors.push({kind,now:performance.now(),message});
  };
  window.addEventListener('error', event => record('error',describe(event.error || event.message)));
  window.addEventListener('unhandledrejection', event => record('unhandledrejection',describe(event.reason)));
  const originalError = console.error;
  console.error = function(...args) {
    record('console.error',args.map(describe).join(' '));
    return originalError.apply(this,args);
  };
}
module.exports = { installApplicationDiagnostics };
