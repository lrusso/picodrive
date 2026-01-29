// Emscripten pre-initialization for PicoDrive
var Module = Module || {};

Module.onRuntimeInitialized = function() {
    console.log('PicoDrive runtime initialized');
    if (typeof window.onPicoReady === 'function') {
        window.onPicoReady();
    }
};

// Memory growth callback
Module.onAbort = function(what) {
    console.error('PicoDrive aborted:', what);
    if (typeof window.onPicoError === 'function') {
        window.onPicoError(what);
    }
};
