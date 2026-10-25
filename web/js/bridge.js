// Loads the WASM module (tsl.js) and turns its callbacks into events.
//
// The module calls window.tslEvent(type, json) for everything that happens on its threads,
// and window.tslReady() once its functions can be called. Opening the page with ?mock
// loads dev/mock.js instead, a stand-in for testing the UI in a desktop browser (the
// dev directory is not packaged into the TV app).
(function () {
    'use strict';

    var listeners = {};
    // A fatal error can come before the UI listens for it (a module that fails as it loads);
    // the UI gets it when it starts listening.
    var fatal = null;
    var resolveReady;
    var ready = new Promise(function (resolve) {
        resolveReady = resolve;
    });

    function dispatch(type, data) {
        if (type === 'fatal' && !fatal) fatal = data;
        var fns = (listeners[type] || []).concat(listeners['*'] || []);
        for (var i = 0; i < fns.length; i++) {
            try {
                fns[i](data, type);
            } catch (e) {
                console.error('Listener for ' + type + ' failed', e);
            }
        }
    }

    window.tslEvent = function (type, json) {
        var data = {};
        if (json) {
            try {
                data = JSON.parse(json);
            } catch (e) {
                console.warn('Unreadable event payload', type, json);
            }
        }
        dispatch(type, data);
    };

    window.tslReady = function () {
        resolveReady(window.Module);
    };

    window.tsl = {
        ready: ready,
        on: function (type, fn) {
            (listeners[type] = listeners[type] || []).push(fn);
            if (type === 'fatal' && fatal) fn(fatal, type);
        },
        emit: dispatch,
    };

    window.Module = {
        locateFile: function (path) {
            return path;
        },
        print: function (text) {
            console.log(text);
        },
        printErr: function (text) {
            console.warn(text);
            // A module thread that crashes (a trap such as "memory access out of bounds") only
            // reaches the page as this line; the module is unusable from then on.
            if (/^pthread sent an error!/.test(text)) {
                dispatch('fatal', {message: 'The streaming module stopped: ' + text, crashed: true});
            }
        },
        onAbort: function (what) {
            dispatch('fatal', {message: 'The streaming module stopped: ' + what});
        },
    };

    var mock = /[?&]mock(&|$)/.test(location.search);
    var script = document.createElement('script');
    script.src = mock ? 'dev/mock.js' : 'tsl.js';
    script.onerror = function () {
        dispatch('fatal', {message: 'Could not load ' + script.src});
    };
    document.head.appendChild(script);
})();
