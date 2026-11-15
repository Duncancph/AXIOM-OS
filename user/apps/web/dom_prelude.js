// Web's browser APIs for page scripts, built on the C primitives in __d (web/dom.c).
// Turned into dom_prelude.h by tools/embed.py at build time.
(function (g) {
  'use strict';
  var D = g.__d;
  delete g.__d;
  var hidden = function (o, k, v) { Object.defineProperty(o, k, { value: v, writable: true, configurable: true, enumerable: false }); };
  // read-only ones: an object may still give itself its own value of that name (frameworks' components do: content, host...)
  var ownSetter = function (k) { return function (v) { Object.defineProperty(this, k, { value: v, writable: true, configurable: true, enumerable: true }); }; };
  var getter = function (o, k, get, set) { Object.defineProperty(o, k, { get: get, set: set || ownSetter(k), configurable: true, enumerable: true }); };

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
  Event.prototype.initCustomEvent = function (t, b, c, d) { this.initEvent(t, b, c); this.detail = d; };   // the old way (createEvent('CustomEvent'))
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
  var MEDIA_EVENTS = { play: 1, playing: 1, pause: 1, ended: 1, timeupdate: 1, durationchange: 1, loadedmetadata: 1, loadeddata: 1, canplay: 1, canplaythrough: 1, loadstart: 1, seeked: 1, volumechange: 1, emptied: 1, error: 1 };
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
  // custom elements: `class X extends HTMLElement` constructors end up here through super()
  function HTMLElement() {
    var nt = new.target;
    if (!nt) throw new TypeError('Illegal constructor');
    var el = ceUpgrading;
    if (el) ceUpgrading = null;
    else {                                              // new X() or createElement('x-y'): a fresh element
      var def = ceByCtor(nt);
      if (!def) throw new TypeError('Illegal constructor');
      el = ceRawCreate(def.name); hidden(el, '_ce', 2);
    }
    Object.setPrototypeOf(el, nt.prototype);
    return el;
  }
  HTMLElement.prototype = Element.prototype;
  var P = Element.prototype, NP = Node.prototype, TP = Text.prototype;

  var FRAG = '#axiom-fragment';
  var ceUpgrading = null, ceRawCreate = D.create, ceRawInsert = D.insert;
  function isFrag(n) { return n && D.type(n) === 1 && D.tag(n) === FRAG; }
  function kids(n) { var a = [], c = D.first(n); while (c) { a.push(c); c = D.next(c); } return a; }
  function elems(n) { return kids(n).filter(function (k) { return D.type(k) === 1; }); }
  function toNode(x) { return x && typeof x === 'object' && D.type(x) ? x : doc.createTextNode(String(x)); }

  getter(NP, 'nodeType', function () { return isFrag(this) || D.host(this) ? 11 : D.type(this); });
  getter(NP, 'nodeName', function () { return D.type(this) === 3 ? '#text' : isFrag(this) || D.host(this) ? '#document-fragment' : D.tag(this).toUpperCase(); });
  getter(NP, 'parentNode', function () { var p = D.parent(this); return p; });
  getter(NP, 'parentElement', function () { var p = D.parent(this); return p && D.type(p) === 1 && !isFrag(p) ? p : null; });
  getter(NP, 'childNodes', function () { return kids(this); });
  getter(NP, 'firstChild', function () { return D.first(this); });
  getter(NP, 'lastChild', function () { return D.last(this); });
  getter(NP, 'nextSibling', function () { return D.next(this); });
  getter(NP, 'previousSibling', function () { return D.prev(this); });
  getter(NP, 'ownerDocument', function () { return doc; });
  function topOf(n) { for (;;) { var p = D.parent(n); if (p) { n = p; continue; } var h = D.host(n); if (h) { n = h; continue; } return n; } }   // across shadow roots
  getter(NP, 'isConnected', function () { return topOf(this) === D.root(); });
  getter(NP, 'textContent', function () { return D.textContent(this); }, function (v) { D.setTextContent(this, v == null ? '' : String(v)); });
  getter(NP, 'nodeValue', function () { return D.type(this) === 3 ? D.data(this) : null; }, function (v) { if (D.type(this) === 3) D.setData(this, String(v)); });
  NP.hasChildNodes = function () { return !!D.first(this); };
  NP.contains = function (o) { while (o) { if (o === this) return true; o = D.parent(o); } return false; };
  NP.getRootNode = function (o) { if (o && o.composed) { var t = topOf(this); return t === D.root() ? doc : t; } var n = this; while (D.parent(n)) n = D.parent(n); return n === D.root() ? doc : n; };
  NP.appendChild = function (n) { return this.insertBefore(n, null); };
  NP.insertBefore = function (n, ref) {
    if (isFrag(n)) { var ks = kids(n); for (var i = 0; i < ks.length; i++) D.insert(this, ks[i], ref || null); return n; }
    D.insert(this, n, ref || null); return n;
  };
  NP.removeChild = function (n) { if (D.parent(n) !== this) throw new Error('NotFoundError: the node is not a child of this node'); D.remove(n); return n; };
  NP.replaceChild = function (n, old) { this.insertBefore(n, old); D.remove(old); return old; };
  // a deep copy also copies the contents of every <template> inside it (they live outside the tree)
  function deepClone(n) {
    var c = D.clone(n, true);
    if (D.type(n) !== 1) return c;
    var a = D.tag(n) === 'template' ? [n] : [], b = D.tag(c) === 'template' ? [c] : [];
    a = a.concat(D.query(n, 'template', true)); b = b.concat(D.query(c, 'template', true));
    for (var i = 0; i < a.length && i < b.length; i++) {
      var src = a[i]._content || (D.first(a[i]) ? tplContent(a[i]) : null);
      if (src) { hidden(b[i], '_content', deepClone(src)); var k; while ((k = D.first(b[i]))) (ceRawInsert || D.insert)(b[i]._content, k, null); }
    }
    return c;
  }
  NP.cloneNode = function (deep) { return deep ? deepClone(this) : D.clone(this, false); };
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
  ['hidden', 'required', 'readOnly', 'multiple', 'autofocus', 'open', 'noValidate', 'draggable', 'selected', 'novalidate'].forEach(function (k) { boolAttr(k, k.toLowerCase()); });
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
  getter(P, 'attributes', function () {                // a NamedNodeMap: a list with lookups by name
    var self = this, l = D.attrNames(this).map(function (k) { return { name: k, localName: k, nodeName: k, value: D.attr(self, k), nodeValue: D.attr(self, k), nodeType: 2, specified: true, ownerElement: self, namespaceURI: null, prefix: null }; });
    l.getNamedItem = function (k) { k = String(k).toLowerCase(); for (var i = 0; i < l.length; i++) if (l[i].name === k) return l[i]; return null; };
    l.getNamedItemNS = function (ns, k) { return l.getNamedItem(k); };
    l.item = function (i) { return l[i] || null; };
    l.setNamedItem = function (at) { self.setAttribute(at.name, at.value); return null; };
    l.removeNamedItem = function (k) { var at = l.getNamedItem(k); self.removeAttribute(k); return at; };
    return l;
  });
  getter(P, 'children', function () { return elems(this); });
  getter(P, 'childElementCount', function () { return elems(this).length; });
  getter(P, 'firstElementChild', function () { return elems(this)[0] || null; });
  getter(P, 'lastElementChild', function () { var e = elems(this); return e[e.length - 1] || null; });
  getter(NP, 'nextElementSibling', function () { var n = D.next(this); while (n && D.type(n) !== 1) n = D.next(n); return n; });
  getter(NP, 'previousElementSibling', function () { var n = D.prev(this); while (n && D.type(n) !== 1) n = D.prev(n); return n; });
  getter(P, 'innerHTML', function () { return D.tag(this) === 'template' ? D.html(tplContent(this), false) : D.html(this, false); },
    function (v) { if (D.tag(this) === 'template') { hidden(this, '_content', null); D.setHtml(this, v == null ? '' : String(v), false); tplContent(this); return; } D.setHtml(this, v == null ? '' : String(v), false); });
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
  // ---- shadow DOM (drawn by Web as the flattened tree: see css_compose) ----
  P.attachShadow = function (init) {
    if (D.shadow(this)) throw new Error('NotSupportedError: this element already has a shadow root');
    var r = D.attachShadow(this);
    hidden(r, '_mode', init && init.mode === 'closed' ? 'closed' : 'open');
    hidden(r, '_delegates', !!(init && init.delegatesFocus));
    if (r._mode === 'closed') hidden(this, '_closedRoot', true);
    return r;
  };
  getter(P, 'shadowRoot', function () { var r = D.shadow(this); return r && !this._closedRoot ? r : null; });
  getter(P, 'slot', function () { return D.attr(this, 'slot') || ''; }, function (v) { D.setAttr(this, 'slot', String(v)); });
  getter(NP, 'assignedSlot', function () {
    var h = D.parent(this), r = h && D.type(h) === 1 ? D.shadow(h) : null; if (!r) return null;
    var nm = D.type(this) === 1 ? (D.attr(this, 'slot') || '') : '';
    var slots = D.query(r, 'slot', true);
    for (var i = 0; i < slots.length; i++) if ((D.attr(slots[i], 'name') || '') === nm) return slots[i];
    return null;
  });
  P.assignedNodes = function (o) {
    var r = this, h; while (D.parent(r)) r = D.parent(r); h = D.host(r);
    if (!h || D.tag(this) !== 'slot') return [];
    var nm = D.attr(this, 'name') || '', out = [];
    kids(h).forEach(function (k) { var ks = D.type(k) === 1 ? (D.attr(k, 'slot') || '') : ''; if (ks === nm && (D.type(k) === 1 || nm === '')) out.push(k); });
    if (!out.length && o && o.flatten) return kids(this);
    return out;
  };
  P.assignedElements = function (o) { return this.assignedNodes(o).filter(function (k) { return D.type(k) === 1; }); };
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
  // <template>: its contents live in a separate, inert fragment (not rendered, custom elements not upgraded)
  function tplContent(t) {
    var f = t._content;
    if (!f) { f = D.create(FRAG); hidden(t, '_content', f); }
    var k, ins = ceRawInsert || D.insert;
    while ((k = D.first(t))) ins(f, k, null);              // what the parser (or innerHTML) put inside
    return f;
  }
  getter(P, 'content', function () { return D.tag(this) === 'template' ? tplContent(this) : undefined; });
  getter(P, 'naturalWidth', function () { return D.rect(this)[2]; });
  getter(P, 'naturalHeight', function () { return D.rect(this)[3]; });
  getter(P, 'complete', function () { return true; });
  getter(P, 'validity', function () { return { valid: true, valueMissing: false }; });
  getter(P, 'validationMessage', function () { return ''; });
  getter(P, 'isContentEditable', function () { return false; });
  // the parts of a link's address (a, area): other objects (frameworks' own shadow roots...) may use these names themselves
  ['protocol', 'hostname', 'host', 'pathname', 'search', 'hash', 'origin', 'port', 'username', 'password'].forEach(function (k) {
    Object.defineProperty(P, k, { configurable: true, enumerable: true,
      get: function () { var t = D.type(this) === 1 ? D.tag(this) : ''; if (t !== 'a' && t !== 'area') return undefined; var h = D.attr(this, 'href'); if (h === null) return ''; try { return new URL(D.resolve(h))[k]; } catch (e) { return ''; } },
      set: function (v) {
        var t = D.type(this) === 1 ? D.tag(this) : '';
        if ((t === 'a' || t === 'area') && k !== 'origin') { try { var u = new URL(this.href); u[k] = v; D.setAttr(this, 'href', u.href); } catch (e) {} return; }
        Object.defineProperty(this, k, { value: v, writable: true, configurable: true, enumerable: true });
      } });
  });
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
  function Range() { this.startContainer = this.endContainer = this.commonAncestorContainer = doc; this.startOffset = this.endOffset = 0; this.collapsed = true; }
  Range.prototype.setStart = function (n, o) { this.startContainer = n; this.startOffset = o; };
  Range.prototype.setEnd = function (n, o) { this.endContainer = n; this.endOffset = o; this.collapsed = false; };
  Range.prototype.setStartBefore = Range.prototype.setStartAfter = Range.prototype.setEndBefore = Range.prototype.setEndAfter = function () {};
  Range.prototype.selectNode = Range.prototype.selectNodeContents = function (n) { this.startContainer = this.endContainer = this.commonAncestorContainer = n; };
  Range.prototype.collapse = function () { this.collapsed = true; };
  Range.prototype.cloneRange = function () { var r = new Range(); for (var k in this) if (Object.prototype.hasOwnProperty.call(this, k)) r[k] = this[k]; return r; };
  Range.prototype.detach = Range.prototype.deleteContents = Range.prototype.insertNode = Range.prototype.surroundContents = function () {};
  Range.prototype.toString = function () { return ''; };
  Range.prototype.getBoundingClientRect = function () { return new g.DOMRect(0, 0, 0, 0); };
  Range.prototype.getClientRects = function () { return []; };
  Range.prototype.createContextualFragment = function (h) { var f = doc.createDocumentFragment(); D.setHtml(f, h, false); return f; };
  Range.START_TO_START = 0; Range.START_TO_END = 1; Range.END_TO_END = 2; Range.END_TO_START = 3;
  g.Range = Range; g.StaticRange = Range;
  doc.createRange = function () { return new Range(); };
  var theSelection = { rangeCount: 0, isCollapsed: true, type: 'None', anchorNode: null, focusNode: null, anchorOffset: 0, focusOffset: 0,
    getRangeAt: function () { return new Range(); }, addRange: function () {}, removeAllRanges: function () {}, removeRange: function () {}, empty: function () {},
    collapse: function () {}, collapseToStart: function () {}, collapseToEnd: function () {}, selectAllChildren: function () {}, extend: function () {}, setBaseAndExtent: function () {},
    containsNode: function () { return false; }, toString: function () { return ''; } };
  g.getSelection = doc.getSelection = function () { return theSelection; }; g.Selection = function () {};
  doc.importNode = function (n, deep) { return deep ? deepClone(n) : D.clone(n, false); };
  doc.adoptNode = function (n) { if (D.parent(n)) D.remove(n); return n; };
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
  g.ErrorEvent = function (t, o) { var e = new Event(t, o); o = o || {}; e.message = o.message || ''; e.filename = o.filename || ''; e.lineno = o.lineno || 0; e.colno = o.colno || 0; e.error = o.error; return e; };
  g.ErrorEvent.prototype = Event.prototype;
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
    var c = D.computed(el) || [16, 0, 0, 0, 0, 0, 1, 0, 0], rgb = function (v) { return 'rgb(' + ((v >> 16) & 255) + ', ' + ((v >> 8) & 255) + ', ' + (v & 255) + ')'; };
    var o = { display: disp >= 0 ? names[disp] : 'none', visibility: c[6] ? 'visible' : 'hidden', opacity: '1',
      position: ['static', 'absolute', 'fixed', 'relative'][c[7]] || 'static', width: el.offsetWidth + 'px', height: el.offsetHeight + 'px',
      fontSize: c[0] + 'px', color: rgb(c[1]), backgroundColor: c[2] ? rgb(c[2]) : 'rgba(0, 0, 0, 0)', fontWeight: c[3] ? '700' : '400',
      fontStyle: c[4] ? 'italic' : 'normal', lineHeight: c[5] ? c[5] + 'px' : 'normal', textAlign: ['left', 'right', 'center', 'justify'][c[8]] || 'left',
      fontFamily: 'Inter, sans-serif', direction: 'ltr', overflow: 'visible', transform: 'none', zIndex: 'auto', boxSizing: 'content-box',
      marginTop: '0px', marginRight: '0px', marginBottom: '0px', marginLeft: '0px', paddingTop: '0px', paddingRight: '0px', paddingBottom: '0px', paddingLeft: '0px',
      borderTopWidth: '0px', borderRightWidth: '0px', borderBottomWidth: '0px', borderLeftWidth: '0px', transitionDuration: '0s', animationName: 'none', animationDuration: '0s' };
    o.getPropertyValue = function (k) { k = String(k); var cam = k.replace(/-([a-z])/g, function (m, x) { return x.toUpperCase(); }); return cam in o && typeof o[cam] === 'string' ? o[cam] : s.getPropertyValue(k) || ''; };
    return o;
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
  g.navigator = { userAgent: 'Mozilla/5.0 (AXIOM; x86_64) Arc/0.4', appName: 'Netscape', appVersion: '5.0 (AXIOM)', platform: 'AXIOM', vendor: 'AXIOM', product: 'Gecko',
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
  function AbortSignal() { throw new TypeError('Illegal constructor'); }
  AbortSignal.prototype = Object.create(EventTarget.prototype); AbortSignal.prototype.constructor = AbortSignal;
  function newSignal() { var s = Object.create(AbortSignal.prototype); s.aborted = false; s.reason = undefined; s.onabort = null; return s; }
  function abortSignal(s, r) {
    if (s.aborted) return;
    s.aborted = true; s.reason = r === undefined ? new Error('AbortError: signal is aborted without reason') : r;
    var ev = new Event('abort'); s.dispatchEvent(ev); if (typeof s.onabort === 'function') s.onabort(ev);
  }
  AbortSignal.prototype.throwIfAborted = function () { if (this.aborted) throw this.reason; };
  AbortSignal.abort = function (r) { var s = newSignal(); abortSignal(s, r); return s; };
  AbortSignal.timeout = function (ms) { var s = newSignal(); D.setTimer(function () { abortSignal(s, new Error('TimeoutError: signal timed out')); }, +ms || 0, false); return s; };
  AbortSignal.any = function (list) { var s = newSignal(); Array.prototype.forEach.call(list || [], function (o) { if (o.aborted) abortSignal(s, o.reason); else o.addEventListener('abort', function () { abortSignal(s, o.reason); }); }); return s; };
  g.AbortSignal = AbortSignal;
  g.AbortController = function () { var s = newSignal(); this.signal = s; this.abort = function (r) { abortSignal(s, r); }; };
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
  // ---------------- custom elements ----------------
  // _ce on an element: 1 while its constructor runs, 2 once upgraded; _cec: connectedCallback has run
  var ceDefs = {}, ceList = [], ceWaiting = {}, ceSel = '';
  function ceByCtor(c) { for (var i = 0; i < ceList.length; i++) if (ceList[i].ctor === c) return ceList[i]; return null; }
  function ceCall(el, name, args) { var f = el[name]; if (typeof f === 'function') { try { f.apply(el, args || []); } catch (e) { reportError(e); } } }
  function ceConnected(el) { return topOf(el) === D.root(); }
  function ceUpgrade(el) {
    if (el._ce) return;
    var def = ceDefs[D.tag(el)]; if (!def) return;
    hidden(el, '_ce', 1);
    ceUpgrading = el;
    try { Reflect.construct(def.ctor, []); } catch (e) { reportError(e); Object.setPrototypeOf(el, def.ctor.prototype); }
    ceUpgrading = null;
    el._ce = 2;
    for (var i = 0; i < def.observed.length; i++) { var v = D.attr(el, def.observed[i]); if (v !== null) ceCall(el, 'attributeChangedCallback', [def.observed[i], null, v, null]); }
  }
  // after nodes were inserted: upgrade what is defined, then tell the connected ones (document order)
  function ceInserted(n) {
    if (!ceList.length || !n || D.type(n) !== 1) return;
    var all = D.customIn(n);
    for (var i = 0; i < all.length; i++) {
      var el = all[i];
      if (!el._ce && !ceDefs[D.tag(el)]) continue;
      if (!el._ce) ceUpgrade(el);
      if (el._ce === 2 && !el._cec && ceConnected(el)) { hidden(el, '_cec', true); ceCall(el, 'connectedCallback'); }
    }
  }
  function ceBeforeRemove(n) {                          // the connected custom elements about to leave
    if (!ceList.length || !n || D.type(n) !== 1) return null;
    return D.customIn(n).filter(function (el) { return el._cec; });
  }
  function ceRemoved(list) {
    if (!list) return;
    for (var i = 0; i < list.length; i++) { var el = list[i]; if (!ceConnected(el)) { el._cec = false; ceCall(el, 'disconnectedCallback'); } }
  }
  // every change to the tree goes through these natives
  (function () {
    var ins = D.insert, rem = D.remove, setHtml = D.setHtml, setText = D.setTextContent, setAttr = D.setAttr, remAttr = D.removeAttr, create = D.create;
    D.insert = function (p, n, ref) {
      if (!ceList.length) return ins(p, n, ref);
      var gone = D.parent(n) ? ceBeforeRemove(n) : null;
      var r = ins(p, n, ref);
      if (gone) ceRemoved(gone);
      ceInserted(n); return r;
    };
    D.remove = function (n) { if (!ceList.length) return rem(n); var gone = ceBeforeRemove(n); var r = rem(n); ceRemoved(gone); return r; };
    D.setHtml = function (el, h, outer) {
      if (!ceList.length) return setHtml(el, h, outer);
      var par = outer ? D.parent(el) : el, gone = ceBeforeRemove(el);
      var before = outer && par ? kids(par) : null;
      var r = setHtml(el, h, outer);
      ceRemoved(gone);
      if (outer && par) { kids(par).forEach(function (k) { if (before.indexOf(k) < 0) ceInserted(k); }); }
      else kids(el).forEach(ceInserted);
      return r;
    };
    D.setTextContent = function (el, t) {
      if (!ceList.length || D.type(el) !== 1) return setText(el, t);
      var gone = []; kids(el).forEach(function (k) { var l = ceBeforeRemove(k); if (l) gone = gone.concat(l); });
      var r = setText(el, t); ceRemoved(gone); return r;
    };
    function attrChanged(el, k, old, v) {
      if (el._ce !== 2) return;
      var def = ceDefs[D.tag(el)]; if (!def || def.observed.indexOf(k) < 0) return;
      ceCall(el, 'attributeChangedCallback', [k, old, v, null]);
    }
    D.setAttr = function (el, k, v) { if (!ceList.length) return setAttr(el, k, v); var old = D.attr(el, k); var r = setAttr(el, k, v); attrChanged(el, String(k).toLowerCase(), old, D.attr(el, k)); return r; };
    D.removeAttr = function (el, k) { if (!ceList.length) return remAttr(el, k); var old = D.attr(el, k); var r = remAttr(el, k); if (old !== null) attrChanged(el, String(k).toLowerCase(), old, null); return r; };
    D.create = function (t) {                          // createElement of a defined name runs its constructor
      var def = ceList.length ? ceDefs[String(t).toLowerCase()] : null;
      if (!def || ceUpgrading) return create(t);
      try { var el = new def.ctor(); if (el && el._ce === 2) return el; } catch (e) { reportError(e); }
      var raw = create(t); ceUpgrade(raw); return raw;
    };
  })();
  var ceRegistry = {
    define: function (name, ctor, opts) {
      name = String(name).toLowerCase();
      if (!/^[a-z][a-z0-9._]*-[a-z0-9._-]*$/.test(name)) throw new SyntaxError("'" + name + "' is not a valid custom element name");
      if (ceDefs[name]) throw new Error("NotSupportedError: '" + name + "' has already been defined");
      if (typeof ctor !== 'function') throw new TypeError('The constructor is not a function');
      var obs = [];
      try { obs = ctor.observedAttributes; obs = obs ? Array.prototype.slice.call(obs).map(function (x) { return String(x).toLowerCase(); }) : []; } catch (e) { obs = []; }
      var def = { name: name, ctor: ctor, observed: obs, ext: opts && opts.extends };
      ceDefs[name] = def; ceList.push(def);
      var found = D.customIn(D.root()).filter(function (el) { return D.tag(el) === name; });   // the ones already in the page
      for (var i = 0; i < found.length; i++) ceInserted(found[i]);
      var w = ceWaiting[name]; if (w) { delete ceWaiting[name]; w.forEach(function (f) { f(ctor); }); }
    },
    get: function (name) { var d = ceDefs[String(name).toLowerCase()]; return d ? d.ctor : undefined; },
    getName: function (ctor) { var d = ceByCtor(ctor); return d ? d.name : null; },
    whenDefined: function (name) {
      name = String(name).toLowerCase();
      if (ceDefs[name]) return Promise.resolve(ceDefs[name].ctor);
      return new Promise(function (res) { (ceWaiting[name] = ceWaiting[name] || []).push(res); });
    },
    upgrade: function (root) { if (ceList.length) ceInserted(root); }
  };
  g.customElements = ceRegistry; g.CustomElementRegistry = function () {};
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
      else ev = new Event(type, { bubbles: type !== 'scroll' && type !== 'resize' && !MEDIA_EVENTS[type], cancelable: true });
      ev.isTrusted = true;
      var ok = (type === 'resize' || type === 'scroll' ? g : target).dispatchEvent(ev);
      if (type === 'focus') target.dispatchEvent(new FocusEvent('focusin', { bubbles: true }));
      return !ok;
    }
  });
  // ---------------- <video> and <audio> (played by Web itself, see web/media.c) ----------------
  var MP = Object.create(P);
  function HTMLMediaElement() { throw new TypeError('Illegal constructor'); }
  HTMLMediaElement.prototype = MP; MP.constructor = HTMLMediaElement;
  MP.play = function () { D.media(this, 'play'); return Promise.resolve(); };
  MP.pause = function () { D.media(this, 'pause'); };
  MP.load = function () { D.media(this, 'load'); };
  MP.canPlayType = function (t) {
    t = String(t).toLowerCase();
    if (/webm|ogg|opus|vp8|vp9|av01|hev1|hvc1|flac|mpegurl|dash/.test(t)) return '';
    return /^(video\/mp4|audio\/mp4|audio\/mpeg|audio\/mp3|audio\/wav|audio\/x-wav|audio\/wave|audio\/aac|audio\/x-m4a|video\/x-m4v)/.test(t) ? (t.indexOf('codecs') >= 0 ? 'probably' : 'maybe') : '';
  };
  MP.addTextTrack = function () { return { mode: 'disabled', cues: [], addCue: function () {} }; };
  ['currentTime', 'volume', 'muted', 'loop'].forEach(function (k) {
    Object.defineProperty(MP, k, { get: function () { return D.media(this, 'get', k); }, set: function (v) { D.media(this, 'set', k, k === 'muted' || k === 'loop' ? (v ? 1 : 0) : +v); }, configurable: true });
  });
  ['duration', 'paused', 'ended', 'readyState', 'networkState', 'videoWidth', 'videoHeight'].forEach(function (k) {
    Object.defineProperty(MP, k, { get: function () { return D.media(this, 'get', k); }, configurable: true });
  });
  Object.defineProperty(MP, 'controls', { get: function () { return this.hasAttribute('controls'); }, set: function (v) { if (v) this.setAttribute('controls', ''); else this.removeAttribute('controls'); D.media(this, 'set', 'controls', v ? 1 : 0); }, configurable: true });
  Object.defineProperty(MP, 'autoplay', { get: function () { return this.hasAttribute('autoplay'); }, set: function (v) { if (v) this.setAttribute('autoplay', ''); else this.removeAttribute('autoplay'); }, configurable: true });
  Object.defineProperty(MP, 'currentSrc', { get: function () { return D.media(this, 'src') || ''; }, configurable: true });
  Object.defineProperty(MP, 'error', { get: function () { return D.media(this, 'get', 'error') ? { code: 4, message: D.media(this, 'errtext') } : null; }, configurable: true });
  Object.defineProperty(MP, 'playbackRate', { get: function () { return 1; }, set: function () {}, configurable: true });
  Object.defineProperty(MP, 'defaultPlaybackRate', { get: function () { return 1; }, set: function () {}, configurable: true });
  Object.defineProperty(MP, 'seeking', { get: function () { return false; }, configurable: true });
  Object.defineProperty(MP, 'preload', { get: function () { return this.getAttribute('preload') || 'metadata'; }, set: function (v) { this.setAttribute('preload', v); }, configurable: true });
  Object.defineProperty(MP, 'poster', { get: function () { return this.getAttribute('poster') || ''; }, set: function (v) { this.setAttribute('poster', v); }, configurable: true });
  function ranges(el) { var d = el.duration; var n = d === d && d > 0 ? 1 : 0; return { length: n, start: function () { return 0; }, end: function () { return n ? el.duration : 0; } }; }
  Object.defineProperty(MP, 'buffered', { get: function () { return ranges(this); }, configurable: true });
  Object.defineProperty(MP, 'seekable', { get: function () { return ranges(this); }, configurable: true });
  Object.defineProperty(MP, 'played', { get: function () { return ranges(this); }, configurable: true });
  Object.defineProperty(MP, 'textTracks', { get: function () { return []; }, configurable: true });
  ['NETWORK_EMPTY', 'NETWORK_IDLE', 'NETWORK_LOADING', 'NETWORK_NO_SOURCE'].forEach(function (k, i) { MP[k] = i; HTMLMediaElement[k] = i; });
  ['HAVE_NOTHING', 'HAVE_METADATA', 'HAVE_CURRENT_DATA', 'HAVE_FUTURE_DATA', 'HAVE_ENOUGH_DATA'].forEach(function (k, i) { MP[k] = i; HTMLMediaElement[k] = i; });
  g.HTMLMediaElement = HTMLMediaElement; g.HTMLVideoElement = HTMLMediaElement; g.HTMLAudioElement = HTMLMediaElement;
  g.Audio = function (src) { var a = doc.createElement('audio'); if (src !== undefined) a.setAttribute('src', src); return a; };
  // ShadowRoot: a document fragment with a host
  var SRP = Object.create(P);
  function ShadowRoot() { throw new TypeError('Illegal constructor'); }
  ShadowRoot.prototype = SRP; SRP.constructor = ShadowRoot;
  getter(SRP, 'host', function () { return D.host(this); });
  getter(SRP, 'mode', function () { return this._mode || 'open'; });
  getter(SRP, 'delegatesFocus', function () { return !!this._delegates; });
  getter(SRP, 'activeElement', function () { var a = doc._active; return a && this.contains(a) ? a : null; });
  getter(SRP, 'parentNode', function () { return null; });
  getter(SRP, 'parentElement', function () { return null; });
  getter(SRP, 'tagName', function () { return undefined; });
  SRP.getElementById = function (id) { return D.query(this, '#' + CSS.escape(String(id)), false)[0] || null; };
  SRP.toString = function () { return '[object ShadowRoot]'; };
  g.ShadowRoot = ShadowRoot;
  // geometry, errors, files, events, style sheets: small working versions
  function DOMRectReadOnly(x, y, w, h) { this.x = +x || 0; this.y = +y || 0; this.width = +w || 0; this.height = +h || 0; this.left = Math.min(this.x, this.x + this.width); this.top = Math.min(this.y, this.y + this.height); this.right = Math.max(this.x, this.x + this.width); this.bottom = Math.max(this.y, this.y + this.height); }
  DOMRectReadOnly.prototype.toJSON = function () { return { x: this.x, y: this.y, width: this.width, height: this.height, top: this.top, left: this.left, right: this.right, bottom: this.bottom }; };
  DOMRectReadOnly.fromRect = function (o) { o = o || {}; return new DOMRectReadOnly(o.x, o.y, o.width, o.height); };
  g.DOMRectReadOnly = DOMRectReadOnly; g.DOMRect = DOMRectReadOnly;
  g.DOMPoint = g.DOMPointReadOnly = function (x, y, z, w) { this.x = +x || 0; this.y = +y || 0; this.z = +z || 0; this.w = w === undefined ? 1 : +w; };
  function DOMMatrix() { this.a = this.d = this.m11 = this.m22 = this.m33 = this.m44 = 1; this.b = this.c = this.e = this.f = 0; this.is2D = true; this.isIdentity = true; }
  DOMMatrix.prototype.translate = DOMMatrix.prototype.scale = DOMMatrix.prototype.rotate = DOMMatrix.prototype.multiply = DOMMatrix.prototype.inverse = function () { return new DOMMatrix(); };
  DOMMatrix.prototype.transformPoint = function (p) { return p; }; DOMMatrix.prototype.toString = function () { return 'matrix(1, 0, 0, 1, 0, 0)'; };
  g.DOMMatrix = g.DOMMatrixReadOnly = g.WebKitCSSMatrix = DOMMatrix;
  function DOMException(msg, name) { var e = new Error(msg === undefined ? '' : String(msg)); Object.setPrototypeOf(e, DOMException.prototype); e.name = name || 'Error'; e.code = { IndexSizeError: 1, NotFoundError: 8, NotSupportedError: 9, InvalidStateError: 11, SyntaxError: 12, InvalidAccessError: 15, TimeoutError: 23, AbortError: 20, DataCloneError: 25, QuotaExceededError: 22, SecurityError: 18, NetworkError: 19 }[e.name] || 0; return e; }
  DOMException.prototype = Object.create(Error.prototype); DOMException.prototype.constructor = DOMException;
  g.DOMException = DOMException;
  function Blob(parts, o) {
    var t = ''; (parts || []).forEach(function (p) { t += p instanceof Blob ? p._t : typeof p === 'string' ? p : p && p.byteLength !== undefined ? String.fromCharCode.apply(null, Array.prototype.slice.call(new Uint8Array(p.buffer || p))) : String(p); });
    hidden(this, '_t', t); this.size = t.length; this.type = o && o.type ? String(o.type).toLowerCase() : '';
  }
  Blob.prototype.text = function () { return Promise.resolve(this._t); };
  Blob.prototype.slice = function (a, b, t) { return new Blob([this._t.slice(a, b)], { type: t || '' }); };
  Blob.prototype.arrayBuffer = function () { var u = new Uint8Array(this._t.length); for (var i = 0; i < u.length; i++) u[i] = this._t.charCodeAt(i) & 255; return Promise.resolve(u.buffer); };
  g.Blob = Blob;
  g.File = function (parts, name, o) { var b = new Blob(parts, o); Object.setPrototypeOf(b, g.File.prototype); b.name = String(name); b.lastModified = (o && o.lastModified) || Date.now(); return b; };
  g.File.prototype = Object.create(Blob.prototype);
  g.FileReader = function () { this.readyState = 0; this.result = null; this.error = null; this.onload = this.onloadend = this.onerror = null; };
  ['readAsText', 'readAsDataURL', 'readAsArrayBuffer', 'readAsBinaryString'].forEach(function (k) {
    g.FileReader.prototype[k] = function (b) {
      var self = this, t = b && b._t || '';
      D.setTimer(function () {
        self.readyState = 2;
        self.result = k === 'readAsDataURL' ? 'data:' + (b.type || 'application/octet-stream') + ';base64,' + g.btoa(t) : k === 'readAsArrayBuffer' ? (function () { var u = new Uint8Array(t.length); for (var i = 0; i < t.length; i++) u[i] = t.charCodeAt(i) & 255; return u.buffer; })() : t;
        var ev = new Event('load'); ev.target = self;
        if (typeof self.onload === 'function') self.onload(ev); if (typeof self.onloadend === 'function') self.onloadend(ev);
      }, 0, false);
    };
  });
  g.FileReader.prototype.abort = function () {}; g.FileReader.prototype.addEventListener = function (t, f) { this['on' + t] = f; };
  g.FileList = function () {};
  g.MessageEvent = function (t, o) { var e = new Event(t, o); o = o || {}; e.data = o.data; e.origin = o.origin || ''; e.source = o.source || null; e.ports = o.ports || []; e.lastEventId = ''; return e; };
  g.MessageEvent.prototype = Event.prototype;
  g.PromiseRejectionEvent = function (t, o) { var e = new Event(t, o); o = o || {}; e.promise = o.promise; e.reason = o.reason; return e; };
  g.PromiseRejectionEvent.prototype = Event.prototype;
  g.DragEvent = g.MouseEvent; g.ClipboardEvent = g.TransitionEvent = g.AnimationEvent = g.PageTransitionEvent = g.HashChangeEvent = g.PopStateEvent = g.StorageEvent = g.BeforeUnloadEvent = g.CompositionEvent = g.SecurityPolicyViolationEvent = Event;
  function CSSStyleSheet() { this.cssRules = []; this.rules = this.cssRules; this.disabled = false; this.media = []; this.ownerNode = null; }
  CSSStyleSheet.prototype.insertRule = function (r, i) { i = i === undefined ? 0 : i; this.cssRules.splice(i, 0, { cssText: String(r) }); return i; };
  CSSStyleSheet.prototype.deleteRule = function (i) { this.cssRules.splice(i, 1); };
  CSSStyleSheet.prototype.addRule = function (sel, st) { this.insertRule(sel + '{' + st + '}', this.cssRules.length); return -1; };
  g.CSSStyleSheet = CSSStyleSheet; g.StyleSheet = CSSStyleSheet;
  g.CSSRule = { STYLE_RULE: 1, CHARSET_RULE: 2, IMPORT_RULE: 3, MEDIA_RULE: 4, FONT_FACE_RULE: 5, PAGE_RULE: 6, KEYFRAMES_RULE: 7, KEYFRAME_RULE: 8, SUPPORTS_RULE: 12 };
  g.History = function () {}; g.History.prototype = g.history;
  g.Screen = function () {}; g.Screen.prototype = g.screen;
  g.Location = function () {}; g.Navigator = function () {}; g.Navigator.prototype = g.navigator;
  g.PerformanceNavigationTiming = g.PerformanceResourceTiming = g.PerformanceEntry = g.PerformanceMark = g.PerformanceMeasure = function () {};
  if (!g.Intl) {                                        // just enough Intl: en-US style numbers and dates
    var pad2 = function (n) { return (n < 10 ? '0' : '') + n; };
    var group = function (s) { var p = s.split('.'), i = p[0], neg = i[0] === '-'; if (neg) i = i.slice(1); i = i.replace(/\B(?=(\d{3})+(?!\d))/g, ','); return (neg ? '-' : '') + i + (p[1] ? '.' + p[1] : ''); };
    var NumberFormat = function (loc, o) { this.o = o || {}; };
    NumberFormat.prototype.format = function (n) {
      var o = this.o; n = +n;
      if (o.notation === 'compact') { var a = Math.abs(n), u = a >= 1e9 ? [1e9, 'B'] : a >= 1e6 ? [1e6, 'M'] : a >= 1e3 ? [1e3, 'K'] : null; if (u) { var v = n / u[0]; return (Math.abs(v) < 10 ? Math.round(v * 10) / 10 : Math.round(v)) + u[1]; } }
      var max = o.maximumFractionDigits !== undefined ? o.maximumFractionDigits : o.style === 'percent' ? 0 : 3, min = o.minimumFractionDigits || 0;
      if (o.style === 'percent') n *= 100;
      var s = n.toFixed(Math.max(min, Math.min(max, 20))); if (max > min && s.indexOf('.') >= 0) { s = s.replace(/0+$/, ''); var d = s.split('.')[1] || ''; while (d.length < min) d += '0'; s = s.split('.')[0] + (d ? '.' + d : ''); }
      s = o.useGrouping === false ? s : group(s);
      if (o.style === 'percent') s += '%'; else if (o.style === 'currency') s = (o.currency === 'EUR' ? '€' : o.currency === 'GBP' ? '£' : '$') + s; else if (o.style === 'unit' && o.unit) s += ' ' + o.unit;
      return s;
    };
    NumberFormat.prototype.formatToParts = function (n) { return [{ type: 'integer', value: this.format(n) }]; };
    NumberFormat.prototype.resolvedOptions = function () { return { locale: 'en-US', numberingSystem: 'latn', style: this.o.style || 'decimal' }; };
    var MON = ['January', 'February', 'March', 'April', 'May', 'June', 'July', 'August', 'September', 'October', 'November', 'December'], DAY = ['Sunday', 'Monday', 'Tuesday', 'Wednesday', 'Thursday', 'Friday', 'Saturday'];
    var DateTimeFormat = function (loc, o) { this.o = o || {}; };
    DateTimeFormat.prototype.format = function (d) {
      d = d === undefined ? new Date() : new Date(d); var o = this.o, parts = [];
      if (o.weekday) parts.push(o.weekday === 'long' ? DAY[d.getDay()] : DAY[d.getDay()].slice(0, 3));
      var hasDate = o.year || o.month || o.day, hasTime = o.hour || o.minute || o.second;
      if (!hasDate && !hasTime && !o.weekday) hasDate = true;
      if (hasDate) {
        if (o.month === 'long' || o.month === 'short') parts.push((o.month === 'long' ? MON[d.getMonth()] : MON[d.getMonth()].slice(0, 3)) + (o.day ? ' ' + d.getDate() : '') + (o.year ? ', ' + d.getFullYear() : ''));
        else parts.push((d.getMonth() + 1) + '/' + d.getDate() + '/' + d.getFullYear());
      }
      if (hasTime) { var h = d.getHours(), h12 = o.hour12 !== false; parts.push((h12 ? (h % 12 || 12) : pad2(h)) + ':' + pad2(d.getMinutes()) + (o.second ? ':' + pad2(d.getSeconds()) : '') + (h12 ? (h < 12 ? ' AM' : ' PM') : '')); }
      return parts.join(', ');
    };
    DateTimeFormat.prototype.formatToParts = function (d) { return [{ type: 'literal', value: this.format(d) }]; };
    DateTimeFormat.prototype.resolvedOptions = function () { return { locale: 'en-US', calendar: 'gregory', numberingSystem: 'latn', timeZone: 'UTC' }; };
    var PluralRules = function (loc, o) { this.o = o || {}; };
    PluralRules.prototype.select = function (n) { if (this.o.type === 'ordinal') { var t = n % 10, h = n % 100; return t === 1 && h !== 11 ? 'one' : t === 2 && h !== 12 ? 'two' : t === 3 && h !== 13 ? 'few' : 'other'; } return n === 1 ? 'one' : 'other'; };
    PluralRules.prototype.resolvedOptions = function () { return { locale: 'en-US', pluralCategories: ['one', 'other'] }; };
    var RelativeTimeFormat = function (loc, o) { this.o = o || {}; };
    RelativeTimeFormat.prototype.format = function (v, unit) { unit = String(unit).replace(/s$/, ''); var a = Math.abs(v), u = a === 1 ? unit : unit + 's'; return v < 0 ? a + ' ' + u + ' ago' : 'in ' + a + ' ' + u; };
    var Collator = function () {}; Collator.prototype.compare = function (a, b) { a = String(a); b = String(b); return a < b ? -1 : a > b ? 1 : 0; };
    var ListFormat = function (loc, o) { this.o = o || {}; }; ListFormat.prototype.format = function (l) { l = Array.prototype.slice.call(l); var w = this.o.type === 'disjunction' ? 'or' : 'and'; return l.length < 2 ? l.join('') : l.length === 2 ? l[0] + ' ' + w + ' ' + l[1] : l.slice(0, -1).join(', ') + ', ' + w + ' ' + l[l.length - 1]; };
    var Locale = function (t) { this.baseName = String(t || 'en-US'); this.language = this.baseName.split('-')[0]; this.region = this.baseName.split('-')[1]; }; Locale.prototype.toString = function () { return this.baseName; };
    var sup = function () { return ['en-US']; };
    [NumberFormat, DateTimeFormat, PluralRules, RelativeTimeFormat, Collator, ListFormat].forEach(function (C) { C.supportedLocalesOf = sup; });
    g.Intl = { NumberFormat: NumberFormat, DateTimeFormat: DateTimeFormat, PluralRules: PluralRules, RelativeTimeFormat: RelativeTimeFormat, Collator: Collator, ListFormat: ListFormat, Locale: Locale,
      getCanonicalLocales: function (l) { return l === undefined ? [] : [].concat(l); }, supportedValuesOf: function () { return []; } };
  }
  // MessageChannel: two ports; a message arrives at the other port a moment later (scripts use it to schedule work)
  function MessagePort() { hidden(this, '_other', null); hidden(this, '_l', []); this.onmessage = null; hidden(this, '_closed', false); }
  MessagePort.prototype.postMessage = function (data) {
    var to = this._other; if (!to || this._closed) return;
    D.setTimer(function () {
      if (to._closed) return;
      var ev = new Event('message'); ev.data = data; ev.ports = []; ev.origin = ''; ev.source = null;
      if (typeof to.onmessage === 'function') { try { to.onmessage(ev); } catch (e) { reportError(e); } }
      to._l.slice().forEach(function (f) { try { f.call(to, ev); } catch (e) { reportError(e); } });
    }, 0, false);
  };
  MessagePort.prototype.addEventListener = function (t, f) { if (t === 'message' && typeof f === 'function') this._l.push(f); };
  MessagePort.prototype.removeEventListener = function (t, f) { var i = this._l.indexOf(f); if (i >= 0) this._l.splice(i, 1); };
  MessagePort.prototype.start = function () {};
  MessagePort.prototype.close = function () { this._closed = true; };
  g.MessagePort = MessagePort;
  g.MessageChannel = function () { var a = new MessagePort(), b = new MessagePort(); a._other = b; b._other = a; this.port1 = a; this.port2 = b; };
  if (!g.BroadcastChannel) g.BroadcastChannel = function (name) { this.name = name; this.onmessage = null; this.postMessage = function () {}; this.close = function () {}; this.addEventListener = function () {}; this.removeEventListener = function () {}; };
  // NodeFilter, TreeWalker, NodeIterator (walk a subtree in document order)
  var NF = { FILTER_ACCEPT: 1, FILTER_REJECT: 2, FILTER_SKIP: 3, SHOW_ALL: 0xFFFFFFFF, SHOW_ELEMENT: 1, SHOW_ATTRIBUTE: 2, SHOW_TEXT: 4, SHOW_CDATA_SECTION: 8,
    SHOW_PROCESSING_INSTRUCTION: 64, SHOW_COMMENT: 128, SHOW_DOCUMENT: 256, SHOW_DOCUMENT_TYPE: 512, SHOW_DOCUMENT_FRAGMENT: 1024 };
  g.NodeFilter = NF;
  function twAccept(w, n) {
    var t = n.nodeType, bit = 1 << (t - 1);
    if (!(w.whatToShow & bit)) return 3;
    var f = w.filter; if (!f) return 1;
    var r = typeof f === 'function' ? f(n) : f.acceptNode(n);
    return r === undefined ? 1 : r;
  }
  function TreeWalker(root, what, filter) { this.root = root; this.whatToShow = what === undefined ? NF.SHOW_ALL : what >>> 0; this.filter = filter || null; this.currentNode = root; }
  function twNext(w, n, skipKids) {                    // the next node in document order inside root
    if (!skipKids) { var c = D.first(n); if (c) return c; }
    while (n && n !== w.root) { var s = D.next(n); if (s) return s; n = D.parent(n); }
    return null;
  }
  TreeWalker.prototype.nextNode = function () {
    var n = this.currentNode, skip = false;
    for (;;) {
      n = twNext(this, n, skip); if (!n) return null;
      var r = twAccept(this, n); skip = r === 2;
      if (r === 1) { this.currentNode = n; return n; }
    }
  };
  TreeWalker.prototype.previousNode = function () {
    var n = this.currentNode;
    while (n && n !== this.root) {
      var p = D.prev(n);
      if (p) { n = p; var l; while ((l = D.last(n))) n = l; } else n = D.parent(n);
      if (n && twAccept(this, n) === 1) { this.currentNode = n; return n; }
    }
    return null;
  };
  TreeWalker.prototype.firstChild = function () { var c = D.first(this.currentNode); while (c && twAccept(this, c) !== 1) c = D.next(c); if (c) this.currentNode = c; return c || null; };
  TreeWalker.prototype.lastChild = function () { var c = D.last(this.currentNode); while (c && twAccept(this, c) !== 1) c = D.prev(c); if (c) this.currentNode = c; return c || null; };
  TreeWalker.prototype.nextSibling = function () { var c = D.next(this.currentNode); while (c && twAccept(this, c) !== 1) c = D.next(c); if (c) this.currentNode = c; return c || null; };
  TreeWalker.prototype.previousSibling = function () { var c = D.prev(this.currentNode); while (c && twAccept(this, c) !== 1) c = D.prev(c); if (c) this.currentNode = c; return c || null; };
  TreeWalker.prototype.parentNode = function () { var n = this.currentNode; while (n && n !== this.root) { n = D.parent(n); if (n && twAccept(this, n) === 1) { this.currentNode = n; return n; } } return null; };
  doc.createTreeWalker = function (root, what, filter) { return new TreeWalker(root, what, filter); };
  doc.createAttribute = function (n) { return { name: String(n).toLowerCase(), localName: String(n).toLowerCase(), value: '', nodeType: 2, specified: true }; };
  P.setAttributeNode = function (at) { if (at && at.name) this.setAttribute(at.name, at.value == null ? '' : at.value); return null; };
  P.getAttributeNode = function (k) { var v = this.getAttribute(k); return v === null ? null : { name: k, localName: k, value: v, nodeType: 2, specified: true }; };
  // document.implementation: a separate, inert document (templates and parsers use it)
  function makeDoc(title) {
    var html = D.create('html'), head = D.create('head'), body = D.create('body');
    D.insert(html, head, null); D.insert(html, body, null);
    if (title) { var t = D.create('title'); D.setTextContent(t, String(title)); D.insert(head, t, null); }
    var d = Object.create(doc);
    hidden(d, 'documentElement', html); hidden(d, 'head', head); hidden(d, 'body', body);
    hidden(d, 'defaultView', null); hidden(d, 'readyState', 'complete');
    d.importNode = d.adoptNode = function (n, deep) { return D.clone(n, deep !== false); };
    d.getElementById = function (id) { return D.query(html, '#' + g.CSS.escape(String(id)), false)[0] || null; };
    d.querySelector = function (s) { return D.query(html, String(s), false)[0] || null; };
    d.querySelectorAll = function (s) { return D.query(html, String(s), true); };
    d.getElementsByTagName = function (t) { return D.query(html, String(t), true); };
    return d;
  }
  doc.implementation = { createHTMLDocument: function (t) { return makeDoc(t); }, createDocument: function () { return makeDoc(''); },
    createDocumentType: function (n) { return { name: n, nodeType: 10 }; }, hasFeature: function () { return true; } };
  doc.createNodeIterator = function (root, what, filter) {
    var w = new TreeWalker(root, what, filter), first = true;
    return { root: root, whatToShow: w.whatToShow, filter: w.filter, detach: function () {},
      nextNode: function () { if (first) { first = false; if (twAccept(w, root) === 1) return root; } return w.nextNode(); },
      previousNode: function () { return w.previousNode(); } };
  };
  g.TreeWalker = TreeWalker;
  ['CDATASection', 'ProcessingInstruction'].forEach(function (k) { var F = function () { throw new TypeError('Illegal constructor'); }; F.prototype = Object.create(CharacterData.prototype); g[k] = F; });
  if (!g.DocumentType) { g.DocumentType = function () { throw new TypeError('Illegal constructor'); }; g.DocumentType.prototype = Object.create(Node.prototype); }
  if (!g.Attr) { g.Attr = function () { throw new TypeError('Illegal constructor'); }; g.Attr.prototype = Object.create(Node.prototype); }
  function SVGElement() { throw new TypeError('Illegal constructor'); }
  SVGElement.prototype = Object.create(P); g.SVGElement = SVGElement; g.SVGSVGElement = SVGElement; g.SVGGraphicsElement = SVGElement;
  if (!g.Window) { g.Window = function Window() { throw new TypeError('Illegal constructor'); }; g.Window.prototype = g; }
  D.setProtos(P, TP, TP, g.__axiom, MP, SRP);
})(this);
