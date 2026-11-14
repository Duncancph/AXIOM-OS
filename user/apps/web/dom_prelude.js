// Web's browser APIs for page scripts, built on the C primitives in __d (web/dom.c).
// Turned into dom_prelude.h by tools/embed.py at build time.
(function (g) {
  'use strict';
  var D = g.__d;
  delete g.__d;
  var hidden = function (o, k, v) { Object.defineProperty(o, k, { value: v, writable: true, configurable: true, enumerable: false }); };
  var getter = function (o, k, get, set) { Object.defineProperty(o, k, { get: get, set: set, configurable: true, enumerable: true }); };

  // ---------------- events ----------------
  function Event(type, init) {
    init = init || {};
    this.type = String(type);
    this.bubbles = !!init.bubbles; this.cancelable = !!init.cancelable; this.composed = !!init.composed;
    this.defaultPrevented = false; this.target = null; this.currentTarget = null; this.eventPhase = 0;
    this.timeStamp = D.viewport ? performanceNow() : 0; this.isTrusted = false;
    hidden(this, '_stop', false); hidden(this, '_stopNow', false);
  }
  Event.prototype.preventDefault = function () { if (this.cancelable) this.defaultPrevented = true; };
  Event.prototype.stopPropagation = function () { this._stop = true; };
  Event.prototype.stopImmediatePropagation = function () { this._stop = true; this._stopNow = true; };
  Event.prototype.composedPath = function () { return this._path || []; };
  Event.prototype.initEvent = function (t, b, c) { this.type = t; this.bubbles = !!b; this.cancelable = !!c; };
  Object.defineProperty(Event.prototype, 'returnValue', { get: function () { return !this.defaultPrevented; }, set: function (v) { if (!v) this.preventDefault(); } });
  Object.defineProperty(Event.prototype, 'srcElement', { get: function () { return this.target; } });
  Event.NONE = 0; Event.CAPTURING_PHASE = 1; Event.AT_TARGET = 2; Event.BUBBLING_PHASE = 3;
  function sub(name, parent, fields) {
    var C = function (type, init) { parent.call(this, type, init); init = init || {}; for (var k in fields) this[k] = k in init ? init[k] : fields[k]; };
    C.prototype = Object.create(parent.prototype); C.prototype.constructor = C;
    Object.defineProperty(C, 'name', { value: name });
    return C;
  }
  var UIEvent = sub('UIEvent', Event, { detail: 0, view: null });
  var MouseEvent = sub('MouseEvent', UIEvent, { clientX: 0, clientY: 0, screenX: 0, screenY: 0, pageX: 0, pageY: 0, offsetX: 0, offsetY: 0, button: 0, buttons: 0, ctrlKey: false, shiftKey: false, altKey: false, metaKey: false, relatedTarget: null });
  var PointerEvent = sub('PointerEvent', MouseEvent, { pointerId: 1, pointerType: 'mouse', isPrimary: true });
  var KeyboardEvent = sub('KeyboardEvent', UIEvent, { key: '', code: '', keyCode: 0, which: 0, charCode: 0, ctrlKey: false, shiftKey: false, altKey: false, metaKey: false, repeat: false, isComposing: false, location: 0 });
  var InputEvent = sub('InputEvent', UIEvent, { data: null, inputType: '', isComposing: false });
  var FocusEvent = sub('FocusEvent', UIEvent, { relatedTarget: null });
  var CustomEvent = sub('CustomEvent', Event, { detail: null });
  var SubmitEvent = sub('SubmitEvent', Event, { submitter: null });
  var ProgressEvent = sub('ProgressEvent', Event, { lengthComputable: false, loaded: 0, total: 0 });

  function listeners(o) { if (!Object.prototype.hasOwnProperty.call(o, '_ls')) hidden(o, '_ls', {}); return o._ls; }
  var EventTarget = function EventTarget() {};
  EventTarget.prototype.addEventListener = function (type, fn, opt) {
    if (!fn) return;
    var cap = typeof opt === 'boolean' ? opt : !!(opt && opt.capture), once = !!(opt && opt.once);
    var l = listeners(this), list = l[type] || (l[type] = []);
    for (var i = 0; i < list.length; i++) if (list[i].fn === fn && list[i].cap === cap) return;
    list.push({ fn: fn, cap: cap, once: once });
    if (opt && opt.signal && opt.signal.addEventListener) { var self = this; opt.signal.addEventListener('abort', function () { self.removeEventListener(type, fn, opt); }); }
  };
  EventTarget.prototype.removeEventListener = function (type, fn, opt) {
    var cap = typeof opt === 'boolean' ? opt : !!(opt && opt.capture), list = listeners(this)[type];
    if (!list) return;
    for (var i = 0; i < list.length; i++) if (list[i].fn === fn && list[i].cap === cap) { list.splice(i, 1); return; }
  };
  function invoke(node, ev, capture) {
    ev.currentTarget = node;
    var list = node._ls && node._ls[ev.type];
    if (list) {
      list = list.slice();
      for (var i = 0; i < list.length; i++) {
        var l = list[i];
        if (ev.eventPhase !== 2 && l.cap !== capture) continue;
        if (l.once) node.removeEventListener(ev.type, l.fn, l.cap);
        try {
          if (typeof l.fn === 'function') l.fn.call(node, ev); else if (l.fn && typeof l.fn.handleEvent === 'function') l.fn.handleEvent(ev);
        } catch (e) { reportError(e); }
        if (ev._stopNow) return;
      }
    }
    if (!capture || ev.eventPhase === 2) {                // on<type> handlers (properties or attributes)
      var h = handlerOf(node, 'on' + ev.type);
      if (h) {
        var r;
        try { r = h.call(node, ev); } catch (e) { reportError(e); }
        if (r === false) ev.preventDefault();
      }
    }
  }
  EventTarget.prototype.dispatchEvent = function (ev) {
    var path = [], n = this;
    while (n) { path.push(n); n = n === doc ? g : n === g ? null : (n.parentNode || (n.nodeType === 1 && n === doc.documentElement ? doc : null)); }
    if (this.nodeType && path[path.length - 1] !== g && this !== g) {
      // detached nodes: just themselves
    }
    ev.target = this; hidden(ev, '_path', path);
    for (var i = path.length - 1; i > 0 && !ev._stop; i--) { ev.eventPhase = 1; invoke(path[i], ev, true); }
    if (!ev._stop) { ev.eventPhase = 2; invoke(path[0], ev, false); }
    if (ev.bubbles) for (var j = 1; j < path.length && !ev._stop; j++) { ev.eventPhase = 3; invoke(path[j], ev, false); }
    ev.eventPhase = 0; ev.currentTarget = null;
    return !ev.defaultPrevented;
  };
  var compiled = {};
  function handlerOf(node, name) {
    var prop = node[name];
    if (typeof prop === 'function') return prop;              // el.onclick = function () {...}
    if (Object.prototype.hasOwnProperty.call(node, '_h' + name)) return node['_h' + name];
    if (node.nodeType === 1) {
      var code = D.attr(node, name);
      if (code !== null && code !== '') {
        var f = D.compile(code);
        hidden(node, '_h' + name, f);
        return f;
      }
    }
    return null;
  }
  function reportError(e) { console.error(e && e.stack ? e.stack : String(e)); }

  // ---------------- nodes ----------------
  function Node() { throw new TypeError('Illegal constructor'); }
  Node.prototype = Object.create(EventTarget.prototype); Node.prototype.constructor = Node;
  var consts = { ELEMENT_NODE: 1, ATTRIBUTE_NODE: 2, TEXT_NODE: 3, CDATA_SECTION_NODE: 4, COMMENT_NODE: 8, DOCUMENT_NODE: 9, DOCUMENT_TYPE_NODE: 10, DOCUMENT_FRAGMENT_NODE: 11,
    DOCUMENT_POSITION_DISCONNECTED: 1, DOCUMENT_POSITION_PRECEDING: 2, DOCUMENT_POSITION_FOLLOWING: 4, DOCUMENT_POSITION_CONTAINS: 8, DOCUMENT_POSITION_CONTAINED_BY: 16 };
  for (var c in consts) { Node[c] = consts[c]; Node.prototype[c] = consts[c]; }
  function CharacterData() { throw new TypeError('Illegal constructor'); }
  CharacterData.prototype = Object.create(Node.prototype);
  function Text() { throw new TypeError('Illegal constructor'); }
  Text.prototype = Object.create(CharacterData.prototype); Text.prototype.constructor = Text;
  function Comment() { throw new TypeError('Illegal constructor'); }
  Comment.prototype = Object.create(CharacterData.prototype);
  function Element() { throw new TypeError('Illegal constructor'); }
  Element.prototype = Object.create(Node.prototype); Element.prototype.constructor = Element;
  function HTMLElement() { throw new TypeError('Illegal constructor'); }
  HTMLElement.prototype = Element.prototype;
  var P = Element.prototype, NP = Node.prototype, TP = Text.prototype;

  var FRAG = '#axiom-fragment';
  function isFrag(n) { return n && n.nodeType === 1 && D.tag(n) === FRAG; }
  function kids(n) { var a = [], c = D.first(n); while (c) { a.push(c); c = D.next(c); } return a; }
  function elems(n) { return kids(n).filter(function (k) { return D.type(k) === 1; }); }
  function toNode(x) { return x && typeof x === 'object' && D.type(x) ? x : doc.createTextNode(String(x)); }

  getter(NP, 'nodeType', function () { return isFrag(this) ? 11 : D.type(this); });
  getter(NP, 'nodeName', function () { return D.type(this) === 3 ? '#text' : isFrag(this) ? '#document-fragment' : D.tag(this).toUpperCase(); });
  getter(NP, 'parentNode', function () { var p = D.parent(this); return p; });
  getter(NP, 'parentElement', function () { var p = D.parent(this); return p && D.type(p) === 1 && !isFrag(p) ? p : null; });
  getter(NP, 'childNodes', function () { return kids(this); });
  getter(NP, 'firstChild', function () { return D.first(this); });
  getter(NP, 'lastChild', function () { return D.last(this); });
  getter(NP, 'nextSibling', function () { return D.next(this); });
  getter(NP, 'previousSibling', function () { return D.prev(this); });
  getter(NP, 'ownerDocument', function () { return doc; });
  getter(NP, 'isConnected', function () { var n = this; while (D.parent(n)) n = D.parent(n); return n === D.root(); });
  getter(NP, 'textContent', function () { return D.textContent(this); }, function (v) { D.setTextContent(this, v == null ? '' : String(v)); });
  getter(NP, 'nodeValue', function () { return D.type(this) === 3 ? D.data(this) : null; }, function (v) { if (D.type(this) === 3) D.setData(this, String(v)); });
  NP.hasChildNodes = function () { return !!D.first(this); };
  NP.contains = function (o) { while (o) { if (o === this) return true; o = D.parent(o); } return false; };
  NP.getRootNode = function () { var n = this; while (D.parent(n)) n = D.parent(n); return n === D.root() ? doc : n; };
  NP.appendChild = function (n) { return this.insertBefore(n, null); };
  NP.insertBefore = function (n, ref) {
    if (isFrag(n)) { var ks = kids(n); for (var i = 0; i < ks.length; i++) D.insert(this, ks[i], ref || null); return n; }
    D.insert(this, n, ref || null); return n;
  };
  NP.removeChild = function (n) { if (D.parent(n) !== this) throw new Error('NotFoundError: the node is not a child of this node'); D.remove(n); return n; };
  NP.replaceChild = function (n, old) { this.insertBefore(n, old); D.remove(old); return old; };
  NP.cloneNode = function (deep) { return D.clone(this, !!deep); };
  NP.isEqualNode = function (o) { return o && D.html(this, true) === D.html(o, true); };
  NP.isSameNode = function (o) { return o === this; };
  NP.compareDocumentPosition = function (o) {
    if (o === this) return 0;
    if (this.contains(o)) return 20; if (o.contains(this)) return 10;
    var all = doc.querySelectorAll('*'), a = all.indexOf(this), b = all.indexOf(o);
    return a < 0 || b < 0 ? 1 : b < a ? 2 : 4;
  };
  NP.normalize = function () {};
  // ChildNode / ParentNode mixins
  NP.remove = function () { if (D.parent(this)) D.remove(this); };
  NP.before = function () { var p = D.parent(this); if (!p) return; for (var i = 0; i < arguments.length; i++) p.insertBefore(toNode(arguments[i]), this); };
  NP.after = function () { var p = D.parent(this); if (!p) return; var ref = D.next(this); for (var i = 0; i < arguments.length; i++) p.insertBefore(toNode(arguments[i]), ref); };
  NP.replaceWith = function () { var p = D.parent(this); if (!p) return; var ref = D.next(this); D.remove(this); for (var i = 0; i < arguments.length; i++) p.insertBefore(toNode(arguments[i]), ref); };
  P.append = function () { for (var i = 0; i < arguments.length; i++) this.appendChild(toNode(arguments[i])); };
  P.prepend = function () { var f = D.first(this); for (var i = 0; i < arguments.length; i++) this.insertBefore(toNode(arguments[i]), f); };
  P.replaceChildren = function () { D.setTextContent(this, ''); this.append.apply(this, arguments); };
  getter(TP, 'data', function () { return D.data(this); }, function (v) { D.setData(this, String(v)); });
  getter(TP, 'length', function () { return D.data(this).length; });
  getter(TP, 'wholeText', function () { return D.data(this); });
  TP.splitText = function (o) { var t = D.data(this); D.setData(this, t.slice(0, o)); var n = doc.createTextNode(t.slice(o)); this.after(n); return n; };
  TP.appendData = function (s) { D.setData(this, D.data(this) + s); };

  // ---- elements ----
  getter(P, 'tagName', function () { return D.tag(this).toUpperCase(); });
  getter(P, 'localName', function () { return D.tag(this); });
  getter(P, 'namespaceURI', function () { return 'http://www.w3.org/1999/xhtml'; });
  var reflect = function (prop, attr) { getter(P, prop, function () { var v = D.attr(this, attr); return v === null ? '' : v; }, function (v) { D.setAttr(this, attr, String(v)); }); };
  ['id', 'title', 'lang', 'dir', 'name', 'alt', 'placeholder', 'type', 'rel', 'target', 'accept', 'pattern', 'autocomplete', 'role', 'download'].forEach(function (k) { reflect(k, k); });
  reflect('className', 'class'); reflect('htmlFor', 'for'); reflect('accessKey', 'accesskey');
  var boolAttr = function (prop, attr) { getter(P, prop, function () { return D.attr(this, attr) !== null; }, function (v) { if (v) D.setAttr(this, attr, ''); else D.removeAttr(this, attr); }); };
  ['hidden', 'required', 'readOnly', 'multiple', 'autofocus', 'open', 'async', 'defer', 'noValidate', 'draggable', 'selected', 'novalidate'].forEach(function (k) { boolAttr(k, k.toLowerCase()); });
  getter(P, 'disabled', function () { return D.disabled(this); }, function (v) { D.setDisabled(this, !!v); });
  var urlAttr = function (prop, attr) { getter(P, prop, function () { var v = D.attr(this, attr); return v === null ? '' : D.resolve(v); }, function (v) { D.setAttr(this, attr, String(v)); }); };
  urlAttr('href', 'href'); urlAttr('src', 'src'); urlAttr('action', 'action');
  getter(P, 'method', function () { return (D.attr(this, 'method') || 'get').toLowerCase(); }, function (v) { D.setAttr(this, 'method', v); });
  getter(P, 'tabIndex', function () { var v = D.attr(this, 'tabindex'); return v === null ? -1 : +v; }, function (v) { D.setAttr(this, 'tabindex', String(v)); });
  P.getAttribute = function (k) { return D.attr(this, String(k)); };
  P.setAttribute = function (k, v) { D.setAttr(this, String(k), String(v)); };
  P.removeAttribute = function (k) { D.removeAttr(this, String(k)); };
  P.hasAttribute = function (k) { return D.attr(this, String(k)) !== null; };
  P.hasAttributes = function () { return D.attrNames(this).length > 0; };
  P.toggleAttribute = function (k, force) { var has = this.hasAttribute(k); if (force === undefined ? has : !force) { this.removeAttribute(k); return false; } this.setAttribute(k, ''); return true; };
  P.getAttributeNames = function () { return D.attrNames(this); };
  P.getAttributeNS = function (ns, k) { return this.getAttribute(k); }; P.setAttributeNS = function (ns, k, v) { this.setAttribute(k, v); };
  getter(P, 'attributes', function () { var self = this; return D.attrNames(this).map(function (k) { return { name: k, localName: k, value: D.attr(self, k), nodeName: k }; }); });
  getter(P, 'children', function () { return elems(this); });
  getter(P, 'childElementCount', function () { return elems(this).length; });
  getter(P, 'firstElementChild', function () { return elems(this)[0] || null; });
  getter(P, 'lastElementChild', function () { var e = elems(this); return e[e.length - 1] || null; });
  getter(NP, 'nextElementSibling', function () { var n = D.next(this); while (n && D.type(n) !== 1) n = D.next(n); return n; });
  getter(NP, 'previousElementSibling', function () { var n = D.prev(this); while (n && D.type(n) !== 1) n = D.prev(n); return n; });
  getter(P, 'innerHTML', function () { return D.html(this, false); }, function (v) { D.setHtml(this, v == null ? '' : String(v), false); });
  getter(P, 'outerHTML', function () { return D.html(this, true); }, function (v) { D.setHtml(this, String(v), true); });
  getter(P, 'innerText', function () { return D.textContent(this); }, function (v) { D.setTextContent(this, v == null ? '' : String(v)); });
  getter(P, 'outerText', function () { return D.textContent(this); });
  P.insertAdjacentHTML = function (where, html) {
    var t = doc.createElement('div'); D.setHtml(t, String(html), false);
    var ks = kids(t);
    where = String(where).toLowerCase();
    var ref = where === 'beforebegin' ? this : where === 'afterbegin' ? D.first(this) : where === 'afterend' ? D.next(this) : null;
    var par = where === 'beforebegin' || where === 'afterend' ? D.parent(this) : this;
    if (!par) return;
    for (var i = 0; i < ks.length; i++) D.insert(par, ks[i], ref);
  };
  P.insertAdjacentElement = function (where, el) {
    where = String(where).toLowerCase();
    if (where === 'beforebegin') this.before(el); else if (where === 'afterbegin') this.prepend(el); else if (where === 'beforeend') this.append(el); else this.after(el);
    return el;
  };
  P.insertAdjacentText = function (where, t) { this.insertAdjacentElement(where, doc.createTextNode(t)); };
  P.querySelector = function (s) { return D.query(this, String(s), false)[0] || null; };
  P.querySelectorAll = function (s) { return nodeList(D.query(this, String(s), true)); };
  P.getElementsByTagName = function (t) { return nodeList(D.query(this, t === '*' ? '*' : String(t), true)); };
  P.getElementsByClassName = function (c) { return nodeList(D.query(this, String(c).trim().split(/\s+/).map(function (x) { return '.' + x; }).join(''), true)); };
  P.matches = function (s) { return D.matches(this, String(s)); };
  P.webkitMatchesSelector = P.msMatchesSelector = P.matches;
  P.closest = function (s) { var n = this; while (n && D.type(n) === 1) { if (D.matches(n, s)) return n; n = D.parent(n); } return null; };
  P.getBoundingClientRect = function () {
    var r = D.rect(this), v = D.viewport();
    var o = { x: r[0], y: r[1] - v[2], width: r[2], height: r[3] };
    o.left = o.x; o.top = o.y; o.right = o.x + o.width; o.bottom = o.y + o.height;
    o.toJSON = function () { return this; };
    return o;
  };
  P.getClientRects = function () { return [this.getBoundingClientRect()]; };
  getter(P, 'offsetWidth', function () { return D.rect(this)[2]; });
  getter(P, 'offsetHeight', function () { return D.rect(this)[3]; });
  getter(P, 'offsetLeft', function () { return D.rect(this)[0]; });
  getter(P, 'offsetTop', function () { return D.rect(this)[1]; });
  getter(P, 'offsetParent', function () { return doc.body; });
  getter(P, 'clientWidth', function () { return this === doc.documentElement ? D.viewport()[0] : D.rect(this)[2]; });
  getter(P, 'clientHeight', function () { return this === doc.documentElement ? D.viewport()[1] : D.rect(this)[3]; });
  getter(P, 'clientTop', function () { return 0; }); getter(P, 'clientLeft', function () { return 0; });
  getter(P, 'scrollWidth', function () { return D.rect(this)[2]; });
  getter(P, 'scrollHeight', function () { return this === doc.documentElement || this === doc.body ? D.viewport()[3] : D.rect(this)[3]; });
  getter(P, 'scrollTop', function () { return this === doc.documentElement || this === doc.body ? D.viewport()[2] : 0; }, function (v) { if (this === doc.documentElement || this === doc.body) D.scrollTo(+v || 0); });
  getter(P, 'scrollLeft', function () { return 0; }, function () {});
  P.scrollIntoView = function () { D.scrollTo(Math.max(0, D.rect(this)[1] - 8)); };
  P.scrollTo = P.scroll = function (x, y) { if (this === doc.documentElement || this === doc.body) g.scrollTo(x, y); };
  P.focus = function () { doc._active = this; };
  P.blur = function () { if (doc._active === this) doc._active = null; };
  P.click = function () { var ev = new MouseEvent('click', { bubbles: true, cancelable: true }); if (this.dispatchEvent(ev)) defaultAction(this); };
  P.attachShadow = function () { return this; };
  P.animate = function () { return { finished: Promise.resolve(), cancel: function () {}, play: function () {}, pause: function () {} }; };
  P.requestFullscreen = function () { return Promise.reject(new Error('not supported')); };
  P.setPointerCapture = P.releasePointerCapture = function () {};
  P.toString = function () { return '[object HTML' + (D.tag(this) === 'a' ? 'Anchor' : '') + 'Element]'; };

  // classList
  function TokenList(el, attr) { hidden(this, '_el', el); hidden(this, '_attr', attr); }
  TokenList.prototype._get = function () { var v = D.attr(this._el, this._attr); return v ? v.trim().split(/\s+/).filter(Boolean) : []; };
  TokenList.prototype._put = function (a) { D.setAttr(this._el, this._attr, a.join(' ')); };
  TokenList.prototype.contains = function (t) { return this._get().indexOf(t) >= 0; };
  TokenList.prototype.add = function () { var a = this._get(); for (var i = 0; i < arguments.length; i++) if (a.indexOf(arguments[i]) < 0) a.push(String(arguments[i])); this._put(a); };
  TokenList.prototype.remove = function () { var r = Array.prototype.slice.call(arguments); this._put(this._get().filter(function (t) { return r.indexOf(t) < 0; })); };
  TokenList.prototype.toggle = function (t, force) { var has = this.contains(t); if (force === undefined ? has : !force) { this.remove(t); return false; } if (!has) this.add(t); return true; };
  TokenList.prototype.replace = function (a, b) { var l = this._get(), i = l.indexOf(a); if (i < 0) return false; l[i] = b; this._put(l); return true; };
  TokenList.prototype.item = function (i) { return this._get()[i] || null; };
  TokenList.prototype.forEach = function (f, t) { this._get().forEach(f, t); };
  TokenList.prototype.toString = function () { return this._get().join(' '); };
  TokenList.prototype.entries = function () { return this._get().entries(); };
  TokenList.prototype.values = function () { return this._get().values(); };
  if (typeof Symbol !== 'undefined' && Symbol.iterator) TokenList.prototype[Symbol.iterator] = function () { return this._get()[Symbol.iterator](); };
  Object.defineProperty(TokenList.prototype, 'length', { get: function () { return this._get().length; } });
  Object.defineProperty(TokenList.prototype, 'value', { get: function () { return D.attr(this._el, this._attr) || ''; }, set: function (v) { D.setAttr(this._el, this._attr, v); } });
  getter(P, 'classList', function () { return new TokenList(this, 'class'); });
  getter(P, 'relList', function () { return new TokenList(this, 'rel'); });

  // style
  function camelToKebab(k) { return k === 'cssFloat' ? 'float' : k.replace(/[A-Z]/g, function (m) { return '-' + m.toLowerCase(); }).replace(/^(webkit|moz|ms)-/, '-$1-'); }
  function parseStyle(s) {
    var o = {}, keys = [];
    String(s || '').split(';').forEach(function (d) { var i = d.indexOf(':'); if (i < 0) return; var k = d.slice(0, i).trim().toLowerCase(), v = d.slice(i + 1).trim(); if (!k) return; if (!(k in o)) keys.push(k); o[k] = v; });
    return { o: o, keys: keys };
  }
  function Style(el) { hidden(this, '_el', el); }
  var SP = Style.prototype;
  SP._read = function () { return parseStyle(D.attr(this._el, 'style')); };
  SP.getPropertyValue = function (k) { var p = this._read(); return p.o[String(k).toLowerCase()] || ''; };
  SP.setProperty = function (k, v, prio) {
    k = String(k).toLowerCase(); var p = this._read();
    if (v === null || v === undefined || v === '') { delete p.o[k]; p.keys = p.keys.filter(function (x) { return x !== k; }); }
    else { if (!(k in p.o)) p.keys.push(k); p.o[k] = String(v) + (prio ? ' !important' : ''); }
    D.setAttr(this._el, 'style', p.keys.filter(function (x) { return x in p.o; }).map(function (x) { return x + ': ' + p.o[x]; }).join('; '));
  };
  SP.removeProperty = function (k) { var v = this.getPropertyValue(k); this.setProperty(k, ''); return v; };
  SP.getPropertyPriority = function () { return ''; };
  SP.item = function (i) { return this._read().keys[i] || ''; };
  Object.defineProperty(SP, 'cssText', { get: function () { return D.attr(this._el, 'style') || ''; }, set: function (v) { D.setAttr(this._el, 'style', String(v)); } });
  Object.defineProperty(SP, 'length', { get: function () { return this._read().keys.length; } });
  ['display', 'visibility', 'color', 'background', 'backgroundColor', 'backgroundImage', 'border', 'borderColor', 'borderWidth', 'borderStyle', 'borderRadius',
    'borderTop', 'borderBottom', 'borderLeft', 'borderRight', 'width', 'height', 'minWidth', 'minHeight', 'maxWidth', 'maxHeight', 'margin', 'marginTop', 'marginBottom',
    'marginLeft', 'marginRight', 'padding', 'paddingTop', 'paddingBottom', 'paddingLeft', 'paddingRight', 'font', 'fontSize', 'fontWeight', 'fontStyle', 'fontFamily',
    'lineHeight', 'textAlign', 'textDecoration', 'textTransform', 'whiteSpace', 'verticalAlign', 'opacity', 'position', 'top', 'left', 'right', 'bottom', 'zIndex',
    'float', 'cssFloat', 'clear', 'overflow', 'overflowX', 'overflowY', 'cursor', 'transform', 'transition', 'animation', 'boxShadow', 'boxSizing', 'flex', 'flexDirection',
    'flexWrap', 'flexGrow', 'flexShrink', 'flexBasis', 'justifyContent', 'alignItems', 'alignSelf', 'gap', 'gridTemplateColumns', 'listStyle', 'listStyleType',
    'letterSpacing', 'wordSpacing', 'wordBreak', 'textOverflow', 'outline', 'pointerEvents', 'userSelect', 'content', 'fill', 'stroke', 'filter', 'order', 'inset',
    'tableLayout', 'borderCollapse', 'borderSpacing', 'textIndent', 'fontVariant', 'backgroundPosition', 'backgroundSize', 'backgroundRepeat', 'willChange', 'resize'
  ].forEach(function (k) {
    Object.defineProperty(SP, k, { get: function () { return this.getPropertyValue(camelToKebab(k)); }, set: function (v) { this.setProperty(camelToKebab(k), v); }, configurable: true, enumerable: true });
  });
  getter(P, 'style', function () {
    var s = new Style(this);
    if (typeof Proxy === 'function') return new Proxy(s, {
      get: function (t, k) { return typeof k === 'string' && !(k in t) && /^[a-z-]/i.test(k) ? t.getPropertyValue(k.indexOf('-') >= 0 ? k : camelToKebab(k)) : t[k]; },
      set: function (t, k, v) { if (typeof k === 'string' && !(k in t)) t.setProperty(k.indexOf('-') >= 0 ? k : camelToKebab(k), v); else t[k] = v; return true; }
    });
    return s;
  }, function (v) { D.setAttr(this, 'style', String(v)); });
  getter(P, 'dataset', function () {
    var el = this, o = {};
    D.attrNames(el).forEach(function (k) { if (k.indexOf('data-') === 0) o[k.slice(5).replace(/-([a-z])/g, function (m, c) { return c.toUpperCase(); })] = D.attr(el, k); });
    var toAttr = function (k) { return 'data-' + String(k).replace(/[A-Z]/g, function (m) { return '-' + m.toLowerCase(); }); };
    if (typeof Proxy === 'function') return new Proxy(o, {
      set: function (t, k, v) { t[k] = String(v); D.setAttr(el, toAttr(k), String(v)); return true; },
      deleteProperty: function (t, k) { delete t[k]; D.removeAttr(el, toAttr(k)); return true; }
    });
    return o;
  });

  // forms and other element kinds
  getter(P, 'value', function () { var t = D.tag(this); return t === 'option' ? (D.attr(this, 'value') !== null ? D.attr(this, 'value') : D.textContent(this).trim()) : D.value(this); }, function (v) { D.setValue(this, v == null ? '' : String(v)); });
  getter(P, 'defaultValue', function () { return D.attr(this, 'value') || ''; }, function (v) { D.setAttr(this, 'value', String(v)); });
  getter(P, 'checked', function () { return D.checked(this); }, function (v) {
    var self = this;
    if (v && D.attr(this, 'type') === 'radio' && D.attr(this, 'name')) doc.querySelectorAll('input[type=radio]').forEach(function (r) { if (r !== self && D.attr(r, 'name') === D.attr(self, 'name')) D.setChecked(r, false); });
    D.setChecked(this, !!v);
  });
  getter(P, 'defaultChecked', function () { return D.attr(this, 'checked') !== null; });
  getter(P, 'selectedIndex', function () { return D.selIndex(this); }, function (v) { D.setSelIndex(this, v | 0); });
  getter(P, 'options', function () { return nodeList(D.query(this, 'option', true)); });
  getter(P, 'selectedOptions', function () { var o = this.options, i = D.selIndex(this); return i >= 0 && o[i] ? [o[i]] : []; });
  getter(P, 'form', function () { return this.closest('form'); });
  getter(P, 'elements', function () { return nodeList(D.query(this, 'input,select,textarea,button,fieldset,output', true)); });
  getter(P, 'labels', function () { return this.id ? doc.querySelectorAll('label[for="' + this.id + '"]') : []; });
  getter(P, 'text', function () { return D.textContent(this); }, function (v) { D.setTextContent(this, String(v)); });
  getter(P, 'content', function () { return this; });
  getter(P, 'naturalWidth', function () { return D.rect(this)[2]; });
  getter(P, 'naturalHeight', function () { return D.rect(this)[3]; });
  getter(P, 'complete', function () { return true; });
  getter(P, 'validity', function () { return { valid: true, valueMissing: false }; });
  getter(P, 'validationMessage', function () { return ''; });
  getter(P, 'isContentEditable', function () { return false; });
  getter(P, 'protocol', function () { return new URL(this.href).protocol; });
  getter(P, 'hostname', function () { return new URL(this.href).hostname; });
  getter(P, 'host', function () { return new URL(this.href).host; });
  getter(P, 'pathname', function () { return new URL(this.href).pathname; });
  getter(P, 'search', function () { return new URL(this.href).search; });
  getter(P, 'hash', function () { return new URL(this.href).hash; });
  getter(P, 'origin', function () { return new URL(this.href).origin; });
  P.checkValidity = P.reportValidity = function () { return true; };
  P.setCustomValidity = function () {};
  P.select = P.setSelectionRange = function () {};
  P.submit = function () { var a = this.getAttribute('action'); var q = new URLSearchParams(); this.elements.forEach(function (e) { if (e.name && !e.disabled && ((e.type !== 'checkbox' && e.type !== 'radio') || e.checked)) q.append(e.name, e.value); }); var u = D.resolve(a || D.url().split('?')[0].split('#')[0]); D.navigate(u + (u.indexOf('?') >= 0 ? '&' : '?') + q.toString()); };
  P.requestSubmit = function () { if (this.dispatchEvent(new SubmitEvent('submit', { bubbles: true, cancelable: true }))) this.submit(); };
  P.reset = function () { this.elements.forEach(function (e) { if (e.type === 'checkbox' || e.type === 'radio') e.checked = e.defaultChecked; else if (e.tagName !== 'SELECT') e.value = e.defaultValue; }); };
  P.play = function () { return Promise.resolve(); }; P.pause = P.load = function () {};
  P.getContext = function () { return null; };
  P.showModal = P.show = function () { this.setAttribute('open', ''); }; P.close = function () { this.removeAttribute('open'); };

  function nodeList(a) { a.item = function (i) { return this[i] || null; }; return a; }

  // ---------------- the document ----------------
  function Document() {}
  Document.prototype = Object.create(Node.prototype); Document.prototype.constructor = Document;
  var doc = Object.create(Document.prototype);
  var DP = Document.prototype;
  getter(DP, 'nodeType', function () { return 9; }); getter(DP, 'nodeName', function () { return '#document'; });
  getter(DP, 'documentElement', function () { return D.root(); });
  getter(DP, 'body', function () { return D.body(); });
  getter(DP, 'head', function () { return D.head(); });
  getter(DP, 'parentNode', function () { return null; });
  getter(DP, 'firstChild', function () { return D.root(); });
  getter(DP, 'childNodes', function () { return [D.root()]; });
  getter(DP, 'children', function () { return [D.root()]; });
  getter(DP, 'title', function () { return D.title(); }, function (v) { D.setTitle(String(v)); });
  getter(DP, 'URL', function () { return D.url(); }); getter(DP, 'documentURI', function () { return D.url(); });
  getter(DP, 'baseURI', function () { return D.base(); });
  getter(DP, 'domain', function () { return location.hostname; });
  getter(DP, 'referrer', function () { return ''; });
  getter(DP, 'location', function () { return location; }, function (v) { location.href = v; });
  getter(DP, 'defaultView', function () { return g; });
  getter(DP, 'activeElement', function () { return this._active || D.body(); });
  getter(DP, 'characterSet', function () { return 'UTF-8'; }); getter(DP, 'charset', function () { return 'UTF-8'; });
  getter(DP, 'contentType', function () { return 'text/html'; });
  getter(DP, 'compatMode', function () { return 'CSS1Compat'; });
  getter(DP, 'hidden', function () { return false; }); getter(DP, 'visibilityState', function () { return 'visible'; });
  getter(DP, 'forms', function () { return this.querySelectorAll('form'); });
  getter(DP, 'images', function () { return this.querySelectorAll('img'); });
  getter(DP, 'links', function () { return this.querySelectorAll('a[href],area[href]'); });
  getter(DP, 'scripts', function () { return this.querySelectorAll('script'); });
  getter(DP, 'scrollingElement', function () { return D.root(); });
  getter(DP, 'fonts', function () { return { ready: Promise.resolve(), load: function () { return Promise.resolve([]); }, check: function () { return true; } }; });
  getter(DP, 'currentScript', function () { return null; });
  var cookies = {};
  getter(DP, 'cookie', function () { return Object.keys(cookies).map(function (k) { return k + '=' + cookies[k]; }).join('; '); },
    function (v) { var p = String(v).split(';')[0], i = p.indexOf('='); if (i > 0) { var k = p.slice(0, i).trim(), val = p.slice(i + 1).trim(); if (/expires=Thu, 01 Jan 1970/i.test(v) || /max-age=0/i.test(v)) delete cookies[k]; else cookies[k] = val; } });
  doc.readyState = 'loading';
  doc.getElementById = function (id) { return D.byId(String(id)); };
  doc.querySelector = function (s) { return D.query(null, String(s), false)[0] || null; };
  doc.querySelectorAll = function (s) { return nodeList(D.query(null, String(s), true)); };
  doc.getElementsByTagName = function (t) { return nodeList(D.query(null, String(t), true)); };
  doc.getElementsByClassName = function (c) { return P.getElementsByClassName.call(D.root(), c); };
  doc.getElementsByName = function (n) { return nodeList(D.query(null, '[name="' + n + '"]', true)); };
  doc.createElement = function (t) { return D.create(String(t)); };
  doc.createElementNS = function (ns, t) { return D.create(String(t)); };
  doc.createTextNode = function (t) { return D.createText(String(t)); };
  doc.createComment = function (t) { return D.createText(''); };
  doc.createDocumentFragment = function () { return D.create(FRAG); };
  doc.createEvent = function (t) { return new (/mouse/i.test(t) ? MouseEvent : /key/i.test(t) ? KeyboardEvent : /custom/i.test(t) ? CustomEvent : Event)(''); };
  doc.createRange = function () { return { setStart: function () {}, setEnd: function () {}, selectNodeContents: function () {}, collapse: function () {}, getBoundingClientRect: function () { return { x: 0, y: 0, width: 0, height: 0, top: 0, left: 0, right: 0, bottom: 0 }; }, createContextualFragment: function (h) { var f = doc.createDocumentFragment(); D.setHtml(f, h, false); return f; } }; };
  doc.importNode = doc.adoptNode = function (n, deep) { return deep === undefined ? n : D.clone(n, !!deep); };
  doc.hasFocus = function () { return true; };
  doc.elementFromPoint = function () { return null; };
  doc.getSelection = function () { return { toString: function () { return ''; }, removeAllRanges: function () {}, addRange: function () {}, rangeCount: 0 }; };
  doc.execCommand = function () { return false; };
  doc.open = function () {}; doc.close = function () {};
  doc.write = doc.writeln = function () {
    var html = Array.prototype.join.call(arguments, '');
    var t = doc.createElement('div'); D.setHtml(t, html, false);
    var ks = kids(t), b = D.body() || D.root();
    for (var i = 0; i < ks.length; i++) D.insert(b, ks[i], null);
  };

  // ---------------- the window ----------------
  var start = D.viewport ? Date.now() : 0;
  function performanceNow() { return Date.now() - start; }
  g.window = g; g.self = g; g.top = g; g.parent = g; g.frames = g; g.globalThis = g;
  g.document = doc;
  ['addEventListener', 'removeEventListener', 'dispatchEvent'].forEach(function (k) { g[k] = EventTarget.prototype[k]; });
  g.Node = Node; g.Element = Element; g.HTMLElement = HTMLElement; g.Text = Text; g.Comment = Comment; g.CharacterData = CharacterData;
  g.Document = Document; g.HTMLDocument = Document; g.DocumentFragment = Element; g.EventTarget = EventTarget;
  ['Div', 'Span', 'Anchor', 'Image', 'Input', 'Button', 'Form', 'Select', 'Option', 'TextArea', 'Paragraph', 'Heading', 'Table', 'TableRow', 'TableCell',
    'UList', 'OList', 'LI', 'Label', 'Canvas', 'Video', 'Audio', 'IFrame', 'Script', 'Style', 'Link', 'Meta', 'Body', 'Head', 'Html', 'Template', 'Dialog', 'Details', 'Media', 'Unknown'
  ].forEach(function (k) { g['HTML' + k + 'Element'] = HTMLElement; });
  g.Event = Event; g.UIEvent = UIEvent; g.MouseEvent = MouseEvent; g.PointerEvent = PointerEvent; g.KeyboardEvent = KeyboardEvent; g.InputEvent = InputEvent;
  g.FocusEvent = FocusEvent; g.CustomEvent = CustomEvent; g.SubmitEvent = SubmitEvent; g.ProgressEvent = ProgressEvent; g.TouchEvent = UIEvent; g.WheelEvent = MouseEvent;
  g.DOMTokenList = TokenList; g.CSSStyleDeclaration = Style;
  g.NodeList = Array; g.HTMLCollection = Array;

  g.setTimeout = function (fn, ms) {
    var args = Array.prototype.slice.call(arguments, 2);
    if (typeof fn !== 'function') { var code = String(fn); fn = function () { (0, eval)(code); }; }
    return D.setTimer(args.length ? function () { fn.apply(g, args); } : fn, +ms || 0, false);
  };
  g.setInterval = function (fn, ms) {
    var args = Array.prototype.slice.call(arguments, 2);
    if (typeof fn !== 'function') { var code = String(fn); fn = function () { (0, eval)(code); }; }
    return D.setTimer(args.length ? function () { fn.apply(g, args); } : fn, +ms || 0, true);
  };
  g.clearTimeout = g.clearInterval = function (id) { if (id) D.clearTimer(id); };
  g.requestAnimationFrame = function (fn) { return D.setTimer(function () { fn(performanceNow()); }, 16, false); };
  g.cancelAnimationFrame = function (id) { D.clearTimer(id); };
  g.requestIdleCallback = function (fn) { return D.setTimer(function () { fn({ didTimeout: false, timeRemaining: function () { return 10; } }); }, 1, false); };
  g.cancelIdleCallback = g.clearTimeout;
  g.setImmediate = function (fn) { return g.setTimeout(fn, 0); };
  g.alert = function (m) { D.alert(m === undefined ? '' : String(m)); };
  g.confirm = function (m) { D.alert(String(m) + '\n\n(Web answered Cancel: it can\'t ask questions yet)'); return false; };
  g.prompt = function () { return null; };
  g.print = g.focus = g.blur = g.stop = function () {};
  g.open = function (u) { if (u) D.navigate(String(u)); return null; };
  g.close = function () {};
  g.getComputedStyle = function (el) {
    var s = el.style, disp = D.display(el);
    var names = ['none', 'block', 'inline', 'inline-block', 'list-item', 'table', 'table-row-group', 'table-row', 'table-cell', 'table-caption', 'inline-table', 'flex', 'inline-flex'];
    return { display: disp >= 0 ? names[disp] : 'none', visibility: 'visible', getPropertyValue: function (k) { return k === 'display' ? this.display : s.getPropertyValue(k); }, opacity: '1', position: 'static', width: el.offsetWidth + 'px', height: el.offsetHeight + 'px' };
  };
  g.matchMedia = function (q) {
    var w = D.viewport()[0], m = false, r;
    if ((r = /min-width:\s*([\d.]+)px/.exec(q))) m = w >= +r[1];
    else if ((r = /max-width:\s*([\d.]+)px/.exec(q))) m = w <= +r[1];
    else if (/prefers-color-scheme:\s*light/.test(q)) m = true;
    return { matches: m, media: q, addListener: function () {}, removeListener: function () {}, addEventListener: function () {}, removeEventListener: function () {}, onchange: null };
  };
  g.scrollTo = g.scroll = function (x, y) { if (x && typeof x === 'object') y = x.top; D.scrollTo(+y || 0); };
  g.scrollBy = function (x, y) { if (x && typeof x === 'object') y = x.top; D.scrollTo(D.viewport()[2] + (+y || 0)); };
  ['innerWidth', 'outerWidth'].forEach(function (k) { getter(g, k, function () { return D.viewport()[0]; }); });
  ['innerHeight', 'outerHeight'].forEach(function (k) { getter(g, k, function () { return D.viewport()[1]; }); });
  getter(g, 'scrollY', function () { return D.viewport()[2]; }); getter(g, 'pageYOffset', function () { return D.viewport()[2]; });
  g.scrollX = 0; g.pageXOffset = 0; g.devicePixelRatio = 1; g.screenX = 0; g.screenY = 0; g.isSecureContext = /^https:/.test(D.url()); g.origin = '';
  g.screen = { width: 1920, height: 1080, availWidth: 1920, availHeight: 1080, colorDepth: 24, pixelDepth: 24, orientation: { type: 'landscape-primary', angle: 0 } };
  g.navigator = { userAgent: 'Mozilla/5.0 (AXIOM; x86_64) Web/0.4', appName: 'Netscape', appVersion: '5.0 (AXIOM)', platform: 'AXIOM', vendor: 'AXIOM', product: 'Gecko',
    language: 'en-GB', languages: ['en-GB', 'en'], onLine: true, cookieEnabled: true, doNotTrack: '1', hardwareConcurrency: 1, maxTouchPoints: 0, webdriver: false,
    clipboard: { writeText: function () { return Promise.resolve(); }, readText: function () { return Promise.resolve(''); } },
    sendBeacon: function () { return true; }, javaEnabled: function () { return false; }, serviceWorker: undefined, geolocation: undefined };
  g.performance = { now: performanceNow, timeOrigin: start, mark: function () {}, measure: function () {}, getEntriesByType: function () { return []; }, getEntriesByName: function () { return []; }, timing: { navigationStart: start } };
  g.history = { length: 1, state: null, scrollRestoration: 'auto',
    pushState: function (s, t, u) { this.state = s; this.length++; }, replaceState: function (s) { this.state = s; },
    back: function () {}, forward: function () {}, go: function () {} };

  // location
  function locPart(k) { return function () { return new URL(D.url())[k]; }; }
  var location = {};
  getter(location, 'href', function () { return D.url(); }, function (v) { D.navigate(String(v)); });
  ['protocol', 'host', 'hostname', 'port', 'pathname', 'search', 'hash', 'origin'].forEach(function (k) {
    getter(location, k, locPart(k), function (v) { var u = new URL(D.url()); u[k] = v; if (k === 'hash') return; D.navigate(u.href); });
  });
  location.assign = location.replace = function (u) { D.navigate(String(u)); };
  location.reload = function () { D.navigate(D.url()); };
  location.toString = function () { return D.url(); };
  getter(g, 'location', function () { return location; }, function (v) { D.navigate(String(v)); });

  // storage
  function Storage(persist) {
    var data = {};
    if (persist) try { data = JSON.parse(D.storageLoad()) || {}; } catch (e) { data = {}; }
    var save = function () { if (persist) D.storageSave(JSON.stringify(data)); };
    var s = {
      getItem: function (k) { k = String(k); return Object.prototype.hasOwnProperty.call(data, k) ? data[k] : null; },
      setItem: function (k, v) { data[String(k)] = String(v); save(); },
      removeItem: function (k) { delete data[String(k)]; save(); },
      clear: function () { data = {}; save(); },
      key: function (i) { return Object.keys(data)[i] || null; }
    };
    Object.defineProperty(s, 'length', { get: function () { return Object.keys(data).length; } });
    if (typeof Proxy === 'function') return new Proxy(s, {
      get: function (t, k) { return k in t ? t[k] : t.getItem(k); },
      set: function (t, k, v) { t.setItem(k, v); return true; },
      deleteProperty: function (t, k) { t.removeItem(k); return true; },
      ownKeys: function () { return Object.keys(data); },
      getOwnPropertyDescriptor: function (t, k) { return Object.prototype.hasOwnProperty.call(data, k) ? { value: data[k], enumerable: true, configurable: true } : undefined; }
    });
    return s;
  }
  g.localStorage = Storage(true);
  g.sessionStorage = Storage(false);

  // ---------------- URL ----------------
  function URLSearchParams(init) {
    hidden(this, '_l', []);
    if (init && typeof init === 'object') { if (Array.isArray(init)) init.forEach(function (p) { this.append(p[0], p[1]); }, this); else for (var k in init) this.append(k, init[k]); }
    else if (init) {
      String(init).replace(/^\?/, '').split('&').forEach(function (p) {
        if (!p) return;
        var i = p.indexOf('='), dec = function (s) { try { return decodeURIComponent(s.replace(/\+/g, ' ')); } catch (e) { return s; } };
        this._l.push([dec(i < 0 ? p : p.slice(0, i)), dec(i < 0 ? '' : p.slice(i + 1))]);
      }, this);
    }
  }
  var UP = URLSearchParams.prototype;
  UP.append = function (k, v) { this._l.push([String(k), String(v)]); this._u && this._u._sync(); };
  UP.set = function (k, v) { this.delete(k); this.append(k, v); };
  UP.get = function (k) { for (var i = 0; i < this._l.length; i++) if (this._l[i][0] === k) return this._l[i][1]; return null; };
  UP.getAll = function (k) { return this._l.filter(function (p) { return p[0] === k; }).map(function (p) { return p[1]; }); };
  UP.has = function (k) { return this.get(k) !== null; };
  UP.delete = function (k) { hidden(this, '_l', this._l.filter(function (p) { return p[0] !== k; })); this._u && this._u._sync(); };
  UP.forEach = function (f, t) { this._l.forEach(function (p) { f.call(t, p[1], p[0], this); }, this); };
  UP.keys = function () { return this._l.map(function (p) { return p[0]; })[Symbol.iterator](); };
  UP.values = function () { return this._l.map(function (p) { return p[1]; })[Symbol.iterator](); };
  UP.entries = function () { return this._l.map(function (p) { return [p[0], p[1]]; })[Symbol.iterator](); };
  UP[Symbol.iterator] = UP.entries;
  UP.sort = function () { this._l.sort(function (a, b) { return a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0; }); };
  UP.toString = function () { var e = function (s) { return encodeURIComponent(s).replace(/%20/g, '+'); }; return this._l.map(function (p) { return e(p[0]) + '=' + e(p[1]); }).join('&'); };
  Object.defineProperty(UP, 'size', { get: function () { return this._l.length; } });
  g.URLSearchParams = URLSearchParams;

  function URL(u, base) {
    u = String(u);
    if (base !== undefined) u = resolveAgainst(u, String(base));
    var m = /^([a-zA-Z][a-zA-Z0-9+.-]*:)(?:\/\/(?:([^:@\/]*)(?::([^@\/]*))?@)?([^:\/?#]*)(?::(\d*))?)?([^?#]*)(\?[^#]*)?(#.*)?$/.exec(u);
    if (!m) throw new TypeError('Invalid URL: ' + u);
    this.protocol = m[1].toLowerCase(); this.username = m[2] || ''; this.password = m[3] || ''; this.hostname = (m[4] || '').toLowerCase();
    this.port = m[5] || ''; this.pathname = m[6] || (m[4] !== undefined ? '/' : ''); this.hash = m[8] && m[8] !== '#' ? m[8] : '';
    if ((this.protocol === 'http:' && this.port === '80') || (this.protocol === 'https:' && this.port === '443')) this.port = '';
    hidden(this, '_sp', new URLSearchParams(m[7] || ''));
    hidden(this._sp, '_u', this);
    hidden(this, '_search', m[7] && m[7] !== '?' ? m[7] : '');
  }
  function resolveAgainst(u, base) {
    if (/^[a-zA-Z][a-zA-Z0-9+.-]*:/.test(u)) return u;
    var b = new URL(base);
    if (u.indexOf('//') === 0) return b.protocol + u;
    if (u[0] === '/') return b.origin + u;
    if (u[0] === '?') return b.origin + b.pathname + u;
    if (u[0] === '#') return b.origin + b.pathname + b.search + u;
    var dir = b.pathname.replace(/[^\/]*$/, ''), parts = (dir + u).split('/'), out = [];
    for (var i = 0; i < parts.length; i++) { if (parts[i] === '..') { if (out.length > 1) out.pop(); } else if (parts[i] !== '.') out.push(parts[i]); }
    if (parts[parts.length - 1] === '..' || parts[parts.length - 1] === '.') out.push('');
    return b.origin + out.join('/');
  }
  var URP = URL.prototype;
  URP._sync = function () { var s = this._sp.toString(); this._search = s ? '?' + s : ''; };
  Object.defineProperty(URP, 'search', { get: function () { return this._search; }, set: function (v) { v = String(v); hidden(this, '_sp', new URLSearchParams(v)); hidden(this._sp, '_u', this); this._search = v && v !== '?' ? (v[0] === '?' ? v : '?' + v) : ''; } });
  Object.defineProperty(URP, 'searchParams', { get: function () { return this._sp; } });
  Object.defineProperty(URP, 'host', { get: function () { return this.hostname + (this.port ? ':' + this.port : ''); }, set: function (v) { var p = String(v).split(':'); this.hostname = p[0]; this.port = p[1] || ''; } });
  Object.defineProperty(URP, 'origin', { get: function () { return this.protocol + '//' + this.host; } });
  Object.defineProperty(URP, 'href', { get: function () { return this.protocol + (this.hostname || this.protocol === 'file:' ? '//' : '') + (this.username ? this.username + (this.password ? ':' + this.password : '') + '@' : '') + this.host + this.pathname + this.search + this.hash; }, set: function (v) { var n = new URL(v); for (var k in n) this[k] = n[k]; hidden(this, '_sp', n._sp); hidden(this, '_search', n._search); } });
  URP.toString = URP.toJSON = function () { return this.href; };
  URL.createObjectURL = function () { return 'blob:axiom'; }; URL.revokeObjectURL = function () {};
  URL.canParse = function (u, b) { try { new URL(u, b); return true; } catch (e) { return false; } };
  g.URL = URL; g.webkitURL = URL;

  // ---------------- networking ----------------
  function Headers(init) {
    hidden(this, '_h', {});
    if (init) { if (init instanceof Headers) init.forEach(function (v, k) { this.set(k, v); }, this); else if (Array.isArray(init)) init.forEach(function (p) { this.append(p[0], p[1]); }, this); else for (var k in init) this.append(k, init[k]); }
  }
  Headers.prototype.append = function (k, v) { k = String(k).toLowerCase(); this._h[k] = k in this._h ? this._h[k] + ', ' + v : String(v); };
  Headers.prototype.set = function (k, v) { this._h[String(k).toLowerCase()] = String(v); };
  Headers.prototype.get = function (k) { k = String(k).toLowerCase(); return k in this._h ? this._h[k] : null; };
  Headers.prototype.has = function (k) { return String(k).toLowerCase() in this._h; };
  Headers.prototype.delete = function (k) { delete this._h[String(k).toLowerCase()]; };
  Headers.prototype.forEach = function (f, t) { for (var k in this._h) f.call(t, this._h[k], k, this); };
  Headers.prototype.entries = Headers.prototype[Symbol.iterator] = function () { var h = this._h; return Object.keys(h).map(function (k) { return [k, h[k]]; })[Symbol.iterator](); };
  Headers.prototype.keys = function () { return Object.keys(this._h)[Symbol.iterator](); };
  function Response(body, init) {
    init = init || {};
    hidden(this, '_body', body == null ? '' : String(body));
    this.status = init.status === undefined ? 200 : init.status; this.statusText = init.statusText || '';
    this.ok = this.status >= 200 && this.status < 300; this.headers = new Headers(init.headers); this.url = init.url || ''; this.redirected = false; this.type = 'basic'; this.bodyUsed = false;
  }
  Response.prototype.text = function () { this.bodyUsed = true; return Promise.resolve(this._body); };
  Response.prototype.json = function () { var b = this._body; this.bodyUsed = true; return new Promise(function (ok) { ok(JSON.parse(b)); }); };
  Response.prototype.clone = function () { return new Response(this._body, { status: this.status, headers: this.headers, url: this.url }); };
  Response.prototype.blob = function () { return Promise.resolve({ size: this._body.length, type: this.headers.get('content-type') || '', text: function () { return Promise.resolve(this._b); }, _b: this._body }); };
  Response.prototype.arrayBuffer = function () { return Promise.reject(new TypeError('arrayBuffer() is not supported')); };
  Response.json = function (d, i) { return new Response(JSON.stringify(d), i); };
  function Request(u, i) { i = i || {}; this.url = typeof u === 'string' ? D.resolve(u) : u.url; this.method = (i.method || (u && u.method) || 'GET').toUpperCase(); this.headers = new Headers(i.headers || (u && u.headers)); this.body = i.body || (u && u.body) || null; }
  function headerText(h) { var s = ''; if (h) new Headers(h).forEach(function (v, k) { s += k + ': ' + v + '\r\n'; }); return s; }
  function bodyText(b) { return b == null ? null : b instanceof URLSearchParams ? b.toString() : typeof b === 'object' && b._fd ? b.toString() : String(b); }
  g.fetch = function (input, init) {
    init = init || {};
    var req = input instanceof Request ? input : new Request(input, init);
    var method = (init.method || req.method || 'GET').toUpperCase(), body = bodyText(init.body !== undefined ? init.body : req.body);
    return new Promise(function (resolve, reject) {
      D.setTimer(function () {
        var r = D.http(method, req.url, body, headerText(init.headers || req.headers));
        if (!r[0]) { reject(new TypeError('Failed to fetch: ' + (r[4] || 'network error'))); return; }
        var res = new Response(r[1], { status: r[0], headers: { 'content-type': r[2] }, url: r[3] });
        resolve(res);
      }, 0, false);
    });
  };
  g.Headers = Headers; g.Response = Response; g.Request = Request;
  function FormData(form) {
    hidden(this, '_fd', true); hidden(this, '_l', new URLSearchParams());
    if (form && form.elements) form.elements.forEach(function (e) { if (e.name && !e.disabled && ((e.type !== 'checkbox' && e.type !== 'radio') || e.checked)) this._l.append(e.name, e.value); }, this);
  }
  ['append', 'set', 'get', 'getAll', 'has', 'delete', 'forEach', 'keys', 'values', 'entries'].forEach(function (k) { FormData.prototype[k] = function () { return this._l[k].apply(this._l, arguments); }; });
  FormData.prototype.toString = function () { return this._l.toString(); };
  FormData.prototype[Symbol.iterator] = function () { return this._l.entries(); };
  g.FormData = FormData;
  function XMLHttpRequest() { this.readyState = 0; this.status = 0; this.statusText = ''; this.responseText = ''; this.response = ''; this.responseType = ''; this.responseURL = ''; this.timeout = 0; this.withCredentials = false; hidden(this, '_h', {}); this.upload = { addEventListener: function () {} }; }
  XMLHttpRequest.prototype = Object.create(EventTarget.prototype);
  XMLHttpRequest.prototype.open = function (m, u, async) { this._m = String(m).toUpperCase(); this._u = String(u); this._async = async !== false; this.readyState = 1; };
  XMLHttpRequest.prototype.setRequestHeader = function (k, v) { this._h[k] = v; };
  XMLHttpRequest.prototype.getResponseHeader = function (k) { return /content-type/i.test(k) ? this._ct || null : null; };
  XMLHttpRequest.prototype.getAllResponseHeaders = function () { return this._ct ? 'content-type: ' + this._ct + '\r\n' : ''; };
  XMLHttpRequest.prototype.overrideMimeType = function () {};
  XMLHttpRequest.prototype.abort = function () { this._aborted = true; };
  XMLHttpRequest.prototype.send = function (body) {
    var x = this;
    var run = function () {
      if (x._aborted) return;
      var r = D.http(x._m, x._u, bodyText(body), headerText(x._h));
      x.status = r[0]; x.statusText = r[0] === 200 ? 'OK' : ''; x.responseText = r[1]; x._ct = r[2]; x.responseURL = r[3]; x.readyState = 4;
      x.response = x.responseType === 'json' ? (function () { try { return JSON.parse(r[1]); } catch (e) { return null; } })() : r[1];
      var fire = function (t) { var ev = new ProgressEvent(t); x.dispatchEvent(ev); var h = x['on' + t]; if (typeof h === 'function') h.call(x, ev); };
      fire('readystatechange'); fire(r[0] ? 'load' : 'error'); fire('loadend');
    };
    if (this._async) D.setTimer(run, 0, false); else run();
  };
  XMLHttpRequest.UNSENT = 0; XMLHttpRequest.OPENED = 1; XMLHttpRequest.HEADERS_RECEIVED = 2; XMLHttpRequest.LOADING = 3; XMLHttpRequest.DONE = 4;
  g.XMLHttpRequest = XMLHttpRequest;
  g.AbortController = function () { var s = Object.create(EventTarget.prototype); s.aborted = false; s.reason = undefined; this.signal = s; this.abort = function (r) { if (s.aborted) return; s.aborted = true; s.reason = r; s.dispatchEvent(new Event('abort')); }; };
  g.AbortSignal = { timeout: function () { return new g.AbortController().signal; }, abort: function () { var c = new g.AbortController(); c.abort(); return c.signal; } };
  g.WebSocket = function () { throw new Error('WebSocket is not supported in Web yet'); };
  g.EventSource = function () { throw new Error('EventSource is not supported in Web yet'); };

  // ---------------- misc ----------------
  var b64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
  g.btoa = function (s) {
    s = String(s); var o = '';
    for (var i = 0; i < s.length; i += 3) {
      var a = s.charCodeAt(i), b = s.charCodeAt(i + 1), c = s.charCodeAt(i + 2);
      if (a > 255 || b > 255 || c > 255) throw new Error('InvalidCharacterError');
      var n = a << 16 | (b || 0) << 8 | (c || 0);
      o += b64[n >> 18 & 63] + b64[n >> 12 & 63] + (i + 1 < s.length ? b64[n >> 6 & 63] : '=') + (i + 2 < s.length ? b64[n & 63] : '=');
    }
    return o;
  };
  g.atob = function (s) {
    s = String(s).replace(/[\s=]/g, ''); var o = '', acc = 0, bits = 0;
    for (var i = 0; i < s.length; i++) { var v = b64.indexOf(s[i]); if (v < 0) throw new Error('InvalidCharacterError'); acc = acc << 6 | v; bits += 6; if (bits >= 8) { bits -= 8; o += String.fromCharCode(acc >> bits & 255); } }
    return o;
  };
  g.TextEncoder = function () { this.encoding = 'utf-8'; };
  g.TextEncoder.prototype.encode = function (s) {
    var u = unescape(encodeURIComponent(String(s === undefined ? '' : s))), a = typeof Uint8Array === 'function' ? new Uint8Array(u.length) : [];
    for (var i = 0; i < u.length; i++) a[i] = u.charCodeAt(i);
    return a;
  };
  g.TextDecoder = function () { this.encoding = 'utf-8'; };
  g.TextDecoder.prototype.decode = function (a) { if (!a) return ''; var s = ''; for (var i = 0; i < a.length; i++) s += String.fromCharCode(a[i]); try { return decodeURIComponent(escape(s)); } catch (e) { return s; } };
  var Observer = function (cb) { this.observe = function () {}; this.unobserve = function () {}; this.disconnect = function () {}; this.takeRecords = function () { return []; }; };
  g.MutationObserver = Observer; g.ResizeObserver = Observer; g.PerformanceObserver = Observer;
  g.IntersectionObserver = function (cb) {
    var self = this, seen = [];
    this.observe = function (el) { if (seen.indexOf(el) >= 0) return; seen.push(el); D.setTimer(function () { var r = el.getBoundingClientRect(); cb([{ target: el, isIntersecting: true, intersectionRatio: 1, boundingClientRect: r, intersectionRect: r, rootBounds: null, time: performanceNow() }], self); }, 0, false); };
    this.unobserve = function (el) { var i = seen.indexOf(el); if (i >= 0) seen.splice(i, 1); };
    this.disconnect = function () { seen = []; }; this.takeRecords = function () { return []; };
  };
  g.DOMParser = function () {};
  g.DOMParser.prototype.parseFromString = function (s) {
    var box = doc.createElement('div'); D.setHtml(box, String(s), false);
    return { body: box, documentElement: box, head: box, querySelector: function (q) { return box.querySelector(q); }, querySelectorAll: function (q) { return box.querySelectorAll(q); }, getElementById: function (id) { return box.querySelector('#' + id); }, getElementsByTagName: function (t) { return box.getElementsByTagName(t); }, title: '' };
  };
  g.Image = function (w, h) { var i = doc.createElement('img'); if (w) i.setAttribute('width', w); if (h) i.setAttribute('height', h); return i; };
  g.Option = function (t, v) { var o = doc.createElement('option'); o.textContent = t || ''; if (v !== undefined) o.setAttribute('value', v); return o; };
  g.customElements = { define: function () {}, get: function () {}, whenDefined: function () { return Promise.resolve(); } };
  g.CSS = { supports: function () { return false; }, escape: function (s) { return String(s).replace(/([^\w-])/g, '\\$1'); } };
  g.crypto = g.crypto || {};
  g.crypto.getRandomValues = g.crypto.getRandomValues || function (a) { for (var i = 0; i < a.length; i++) a[i] = Math.floor(Math.random() * 256); return a; };
  g.crypto.randomUUID = g.crypto.randomUUID || function () { return 'xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx'.replace(/[xy]/g, function (c) { var r = Math.random() * 16 | 0; return (c === 'x' ? r : r & 3 | 8).toString(16); }); };
  g.reportError = reportError;
  g.onerror = null;

  // ---------------- what the browser calls ----------------
  function defaultAction(el) {
    for (var n = el; n && n.nodeType === 1; n = D.parent(n)) {
      var t = D.tag(n);
      if (t === 'a' && D.attr(n, 'href') !== null) { var h = D.attr(n, 'href'); if (/^javascript:/i.test(h)) { try { (0, eval)(decodeURIComponent(h.slice(11))); } catch (e) { reportError(e); } } else if (h[0] !== '#' || h.length > 1) D.navigate(D.resolve(h)); return; }
      if ((t === 'button' && (D.attr(n, 'type') || 'submit') === 'submit') || (t === 'input' && /^(submit|image)$/i.test(D.attr(n, 'type') || ''))) { var f = n.closest('form'); if (f) f.requestSubmit(); return; }
    }
  }
  var keyNames = { 8: 'Backspace', 9: 'Tab', 10: 'Enter', 13: 'Enter', 27: 'Escape', 32: ' ' };
  var codeKeys = { 0x101: 'ArrowUp', 0x102: 'ArrowDown', 0x103: 'ArrowLeft', 0x104: 'ArrowRight', 0x105: 'Home', 0x106: 'End', 0x107: 'PageUp', 0x108: 'PageDown', 0x109: 'Delete', 0x10A: 'Insert' };
  var keyCodes = { Backspace: 8, Tab: 9, Enter: 13, Escape: 27, ' ': 32, PageUp: 33, PageDown: 34, End: 35, Home: 36, ArrowLeft: 37, ArrowUp: 38, ArrowRight: 39, ArrowDown: 40, Insert: 45, Delete: 46 };
  hidden(g, '__axiom', {
    start: function () { },
    loaded: function () {
      doc.readyState = 'interactive';
      doc.dispatchEvent(new Event('readystatechange'));
      doc.dispatchEvent(new Event('DOMContentLoaded', { bubbles: true }));
      doc.readyState = 'complete';
      doc.dispatchEvent(new Event('readystatechange'));
      var ev = new Event('load'); g.dispatchEvent(ev);
      var b = D.body(); if (b) { var h = handlerOf(b, 'onload'); if (h && !g.onload) try { h.call(g, ev); } catch (e) { reportError(e); } }
      g.dispatchEvent(new Event('pageshow'));
    },
    // the browser reports a user action; returns true if a script cancelled the default action
    dispatch: function (target, type, x, y, key) {
      var ev;
      if (type === 'click' || type === 'mousedown' || type === 'mouseup' || type === 'dblclick' || type === 'contextmenu') {
        var v = D.viewport();
        ev = new MouseEvent(type, { bubbles: true, cancelable: true, clientX: x, clientY: y - v[2], pageX: x, pageY: y, screenX: x, screenY: y, button: type === 'contextmenu' ? 2 : 0, buttons: 1, view: g, detail: 1 });
        if (type === 'mousedown') target.dispatchEvent(new PointerEvent('pointerdown', { bubbles: true, cancelable: true, clientX: x, clientY: y - v[2] }));
        if (type === 'mouseup') target.dispatchEvent(new PointerEvent('pointerup', { bubbles: true, cancelable: true, clientX: x, clientY: y - v[2] }));
      } else if (type === 'keydown' || type === 'keyup' || type === 'keypress') {
        var k = keyNames[key & 0xFF] && !(key & 0x100) ? keyNames[key & 0xFF] : codeKeys[key & 0x1FF] || String.fromCharCode(key & 0xFF);
        var kc = keyCodes[k] || (k.length === 1 ? k.toUpperCase().charCodeAt(0) : 0);
        ev = new KeyboardEvent(type, { bubbles: true, cancelable: true, key: k, code: k.length === 1 ? (/[a-z]/i.test(k) ? 'Key' + k.toUpperCase() : /[0-9]/.test(k) ? 'Digit' + k : 'Unidentified') : k, keyCode: kc, which: kc, charCode: type === 'keypress' ? k.charCodeAt(0) : 0, shiftKey: !!(key & 0x1000), ctrlKey: !!(key & 0x2000), altKey: !!(key & 0x4000), view: g });
        if (target === D.body() || !target) target = doc.activeElement || D.body();
      } else if (type === 'input') ev = new InputEvent('input', { bubbles: true, inputType: 'insertText' });
      else if (type === 'submit') ev = new SubmitEvent('submit', { bubbles: true, cancelable: true });
      else if (type === 'focus' || type === 'blur') { ev = new FocusEvent(type, {}); if (type === 'focus') doc._active = target; }
      else ev = new Event(type, { bubbles: type !== 'scroll' && type !== 'resize', cancelable: true });
      ev.isTrusted = true;
      var ok = (type === 'resize' || type === 'scroll' ? g : target).dispatchEvent(ev);
      if (type === 'focus') target.dispatchEvent(new FocusEvent('focusin', { bubbles: true }));
      return !ok;
    }
  });
  D.setProtos(P, TP, TP, g.__axiom);
})(this);
