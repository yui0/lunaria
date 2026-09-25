/*
 * luna-browser.js — the web platform scripts see, on top of __lb.
 *
 * Copyright © 2026 Yuichiro Nakada / Project Vespera — MPL 2.0
 *
 * Rendered elements live in luna-ui and are named by uid; a wrapper object
 * per uid gives them identity.  Elements luna-ui does not render (html, head,
 * script, style, link, meta, title, template, fragments, comments) are plain
 * script objects ("virtual" nodes).  Text nodes inserted next to elements
 * become <span>s, which is how they get painted.
 */
(function (g) {
'use strict';
const L = __lb;
delete g.__lb;

const T0 = L.now();
/* The engine's own Promise: a page may replace the global one with a
 * polyfill built on queueMicrotask, which must not come back here. */
const NativePromise = Promise;
const hide = (o, k, v) => Object.defineProperty(o, k, { value: v, writable: true, configurable: true });

/* ---------------------------------------------------------------- console */

function fmt(args) {
   return Array.prototype.map.call(args, a => {
      if (typeof a === 'string') return a;
      if (a instanceof Error) return a.name + ': ' + a.message + (a.stack ? '\n' + a.stack : '');
      if (a && a._uid !== undefined) return '<' + a.localName + '>';
      try { const s = JSON.stringify(a); return s === undefined ? String(a) : s; }
      catch (e) { return String(a); }
   }).join(' ');
}
const timers_label = {};
g.console = {
   log() { L.log('log', fmt(arguments)); },
   info() { L.log('info', fmt(arguments)); },
   debug() { L.log('debug', fmt(arguments)); },
   warn() { L.log('warn', fmt(arguments)); },
   error() { L.log('error', fmt(arguments)); },
   trace() { L.log('debug', fmt(arguments) + '\n' + new Error().stack); },
   dir(o) { L.log('log', fmt([o])); },
   table(o) { L.log('log', fmt([o])); },
   assert(c, ...a) { if (!c) L.log('error', 'Assertion failed: ' + fmt(a)); },
   group() { L.log('log', fmt(arguments)); }, groupCollapsed() {}, groupEnd() {},
   time(l = 'default') { timers_label[l] = L.now(); },
   timeEnd(l = 'default') { L.log('log', l + ': ' + (L.now() - (timers_label[l] || L.now())) + 'ms'); },
   count() {},
};

/* ----------------------------------------------------------------- events */

class Event {
   constructor(type, init = {}) {
      this.type = String(type);
      this.bubbles = !!init.bubbles;
      this.cancelable = !!init.cancelable;
      this.composed = !!init.composed;
      this.defaultPrevented = false;
      this.isTrusted = false;
      this.target = null;
      this.currentTarget = null;
      this.eventPhase = 0;
      this.timeStamp = L.now() - T0;
      hide(this, '_stop', false);
      hide(this, '_stopNow', false);
      hide(this, '_path', []);
   }
   preventDefault() { if (this.cancelable) this.defaultPrevented = true; }
   stopPropagation() { this._stop = true; }
   stopImmediatePropagation() { this._stop = this._stopNow = true; }
   composedPath() { return this._path.slice(); }
   initEvent(type, bubbles, cancelable) { this.type = type; this.bubbles = !!bubbles; this.cancelable = !!cancelable; }
   get returnValue() { return !this.defaultPrevented; }
   set returnValue(v) { if (!v) this.preventDefault(); }
   get srcElement() { return this.target; }
}
Event.NONE = 0; Event.CAPTURING_PHASE = 1; Event.AT_TARGET = 2; Event.BUBBLING_PHASE = 3;
class CustomEvent extends Event { constructor(t, i = {}) { super(t, i); this.detail = i.detail === undefined ? null : i.detail; } }
class UIEvent extends Event { constructor(t, i = {}) { super(t, i); this.view = g; this.detail = i.detail || 0; } }
class MouseEvent extends UIEvent {
   constructor(t, i = {}) {
      super(t, i);
      for (const k of ['clientX', 'clientY', 'screenX', 'screenY', 'pageX', 'pageY', 'offsetX', 'offsetY',
                       'button', 'buttons', 'movementX', 'movementY'])
         this[k] = i[k] || 0;
      if (!i.pageX) this.pageX = this.clientX;
      if (!i.pageY) this.pageY = this.clientY;
      this.x = this.clientX; this.y = this.clientY;
      for (const k of ['ctrlKey', 'shiftKey', 'altKey', 'metaKey']) this[k] = !!i[k];
      this.relatedTarget = i.relatedTarget || null;
   }
   getModifierState() { return false; }
}
class PointerEvent extends MouseEvent {
   constructor(t, i = {}) {
      super(t, i);
      this.pointerId = i.pointerId || 1; this.pointerType = i.pointerType || 'mouse';
      this.isPrimary = true; this.width = 1; this.height = 1; this.pressure = i.pressure || 0;
   }
}
class WheelEvent extends MouseEvent {
   constructor(t, i = {}) { super(t, i); this.deltaX = i.deltaX || 0; this.deltaY = i.deltaY || 0; this.deltaZ = 0; this.deltaMode = 0; }
}
class KeyboardEvent extends UIEvent {
   constructor(t, i = {}) {
      super(t, i);
      this.key = i.key || ''; this.code = i.code || ''; this.keyCode = this.which = i.keyCode || 0;
      this.charCode = i.charCode || 0; this.repeat = !!i.repeat; this.location = 0; this.isComposing = false;
      for (const k of ['ctrlKey', 'shiftKey', 'altKey', 'metaKey']) this[k] = !!i[k];
   }
   getModifierState() { return false; }
}
class FocusEvent extends UIEvent { constructor(t, i = {}) { super(t, i); this.relatedTarget = i.relatedTarget || null; } }
class InputEvent extends UIEvent { constructor(t, i = {}) { super(t, i); this.data = i.data || null; this.inputType = i.inputType || 'insertText'; this.isComposing = false; } }
class Touch {
   constructor(i) { Object.assign(this, { identifier: 0, clientX: 0, clientY: 0, pageX: 0, pageY: 0, screenX: 0, screenY: 0, radiusX: 1, radiusY: 1, force: 1 }, i); }
}
class TouchEvent extends UIEvent {
   constructor(t, i = {}) {
      super(t, i);
      this.touches = i.touches || []; this.targetTouches = i.targetTouches || this.touches;
      this.changedTouches = i.changedTouches || [];
      for (const k of ['ctrlKey', 'shiftKey', 'altKey', 'metaKey']) this[k] = false;
   }
}
class MessageEvent extends Event { constructor(t, i = {}) { super(t, i); this.data = i.data; this.origin = i.origin || ''; this.source = i.source || null; this.ports = []; } }
class ProgressEvent extends Event { constructor(t, i = {}) { super(t, i); this.loaded = i.loaded || 0; this.total = i.total || 0; this.lengthComputable = !!i.lengthComputable; } }
class ErrorEvent extends Event { constructor(t, i = {}) { super(t, i); this.message = i.message || ''; this.error = i.error || null; } }
class PopStateEvent extends Event { constructor(t, i = {}) { super(t, i); this.state = i.state === undefined ? null : i.state; } }

const inlineCache = new Map();

class EventTarget {
   addEventListener(type, fn, opt) {
      if (!fn) return;
      const capture = typeof opt === 'boolean' ? opt : !!(opt && opt.capture);
      const once = !!(opt && typeof opt === 'object' && opt.once);
      if (!this._ls) hide(this, '_ls', Object.create(null));
      const list = this._ls[type] || (this._ls[type] = []);
      if (list.some(l => l.fn === fn && l.capture === capture)) return;
      list.push({ fn, capture, once });
      if (opt && typeof opt === 'object' && opt.signal)
         opt.signal.addEventListener('abort', () => this.removeEventListener(type, fn, opt));
   }
   removeEventListener(type, fn, opt) {
      const capture = typeof opt === 'boolean' ? opt : !!(opt && opt.capture);
      const list = this._ls && this._ls[type];
      if (!list) return;
      const i = list.findIndex(l => l.fn === fn && l.capture === capture);
      if (i >= 0) list.splice(i, 1);
   }
   dispatchEvent(ev) { dispatch(this, ev); return !ev.defaultPrevented; }
}

function invoke(node, ev, capturePhase) {
   const list = node._ls && node._ls[ev.type];
   if (list) {
      for (const l of list.slice()) {
         if (ev._stopNow) return;
         if (ev.eventPhase !== 2 && l.capture !== capturePhase) continue;
         if (l.once) node.removeEventListener(ev.type, l.fn, l.capture);
         try {
            if (typeof l.fn === 'function') l.fn.call(node, ev);
            else if (l.fn && typeof l.fn.handleEvent === 'function') l.fn.handleEvent(ev);
         } catch (e) { reportError(e); }
      }
   }
   if (capturePhase && ev.eventPhase !== 2) return;
   /* on<type> property, then the element's inline attribute */
   let h = node['on' + ev.type];
   if (typeof h !== 'function' && node._uid) {
      const src = L.getAttr(node._uid, 'on' + ev.type);
      if (src) {
         const key = node._uid + ':' + ev.type + ':' + src;
         h = inlineCache.get(key);
         if (!h) {
            try { h = new Function('event', src); } catch (e) { reportError(e); h = null; }
            inlineCache.set(key, h);
         }
      }
   }
   if (typeof h === 'function' && !ev._stopNow) {
      try {
         const r = h.call(node, ev);
         if (r === false) ev.preventDefault();
      } catch (e) { reportError(e); }
   }
}

function eventPath(target) {
   const path = [];
   for (let n = target; n; n = n === document ? g : n.parentNode) {
      path.push(n);
      if (n === g) break;
   }
   return path;
}

function dispatch(target, ev) {
   ev.target = target;
   ev._path = eventPath(target);
   const path = ev._path;
   ev.eventPhase = 1;
   for (let i = path.length - 1; i > 0 && !ev._stop; --i) { ev.currentTarget = path[i]; invoke(path[i], ev, true); }
   if (!ev._stop) { ev.eventPhase = 2; ev.currentTarget = target; invoke(target, ev, true); }
   if (ev.bubbles) {
      ev.eventPhase = 3;
      for (let i = 1; i < path.length && !ev._stop; ++i) { ev.currentTarget = path[i]; invoke(path[i], ev, false); }
   }
   ev.eventPhase = 0;
   ev.currentTarget = null;
   return !ev.defaultPrevented;
}

function reportError(e) {
   L.log('error', 'Uncaught ' + (e && e.stack ? e.name + ': ' + e.message + '\n' + e.stack : String(e)));
   try { dispatch(g, new ErrorEvent('error', { message: String(e && e.message || e), error: e })); } catch (x) {}
}
g.reportError = reportError;

/* ------------------------------------------------------------------ nodes */

const wrappers = new Map();
function W(uid) {
   if (!uid) return null;
   let w = wrappers.get(uid);
   if (!w) {
      const tag = L.tag(uid) || 'div';
      const C = tag === '#text' ? Text : TAGS[tag] || HTMLElement;
      w = Object.create(C.prototype);
      hide(w, '_uid', uid);
      wrappers.set(uid, w);
   }
   return w;
}

class NodeList extends Array {
   item(i) { return this[i] || null; }
   namedItem(n) { return this.find(e => e.id === n || e.getAttribute('name') === n) || null; }
}
const list = a => { const l = new NodeList(); for (const x of a) l.push(x); return l; };

class Node extends EventTarget {
   get ownerDocument() { return this === document ? null : document; }
   get isConnected() {
      if (this._uid) return L.connected(this._uid);
      for (let n = this; n; n = n.parentNode) if (n === document) return true;
      return false;
   }
   get parentElement() { const p = this.parentNode; return p && p.nodeType === 1 ? p : null; }
   get firstChild() { return this.childNodes[0] || null; }
   get lastChild() { const c = this.childNodes; return c[c.length - 1] || null; }
   get nextSibling() { const p = this.parentNode; if (!p) return null; const c = p.childNodes; return c[c.indexOf(this) + 1] || null; }
   get previousSibling() { const p = this.parentNode; if (!p) return null; const c = p.childNodes; const i = c.indexOf(this); return i > 0 ? c[i - 1] : null; }
   hasChildNodes() { return this.childNodes.length > 0; }
   contains(n) { for (; n; n = n.parentNode) if (n === this) return true; return false; }
   getRootNode() { let n = this; while (n.parentNode) n = n.parentNode; return n; }
   isSameNode(n) { return n === this; }
   isEqualNode(n) { return n === this || (n && n.outerHTML !== undefined && n.outerHTML === this.outerHTML); }
   compareDocumentPosition(o) {
      if (o === this) return 0;
      if (this.contains(o)) return 20;
      if (o.contains(this)) return 10;
      const a = document.querySelectorAll('*'), i = a.indexOf(this), j = a.indexOf(o);
      return i < 0 || j < 0 ? 1 : j > i ? 4 : 2;
   }
   normalize() {}
   appendChild(c) { return this.insertBefore(c, null); }
   removeChild(c) {
      if (!c || c.parentNode !== this) throw new DOMException('The node to be removed is not a child of this node.', 'NotFoundError');
      detach(c);
      return c;
   }
   replaceChild(n, old) { this.insertBefore(n, old); this.removeChild(old); return old; }
   insertBefore(c, ref) {
      if (!c) throw new TypeError('insertBefore: not a node');
      if (c.nodeType === 11) {
         for (const k of c.childNodes.slice()) this.insertBefore(k, ref);
         return c;
      }
      if (c.contains(this)) throw new DOMException('The new child element contains the parent.', 'HierarchyRequestError');
      if (ref && ref.parentNode !== this) ref = null;
      detach(c);
      if (c.nodeType === 3 && this._uid) {         /* text into a rendered element */
         if (!c._uid && !L.children(this._uid).length && !this._vkids && L.text(this._uid) === '' && !ref) {
            hide(c, '_host', this);
            L.setText(this._uid, c._data);
            return c;
         }
         c._materialize();
      }
      if (c._uid && this._uid) {
         const before = ref && ref._uid ? ref._uid : 0;
         L.insert(this._uid, c._uid, before);
      } else {
         /* A virtual parent, or a virtual child of a rendered parent. */
         if (!this._vkids) hide(this, '_vkids', []);
         const i = ref ? this._vkids.indexOf(ref) : -1;
         if (i >= 0) this._vkids.splice(i, 0, c); else this._vkids.push(c);
         hide(c, '_vparent', this);
         if (c._uid && !this._uid) L.remove(c._uid);   /* kept, not rendered */
      }
      connected(c);
      return c;
   }
   append(...ns) { for (const n of ns) this.appendChild(typeof n === 'string' ? document.createTextNode(n) : n); }
   prepend(...ns) { const f = this.firstChild; for (const n of ns) this.insertBefore(typeof n === 'string' ? document.createTextNode(n) : n, f); }
   replaceChildren(...ns) { for (const c of this.childNodes.slice()) this.removeChild(c); this.append(...ns); }
   cloneNode(deep) {
      if (this.nodeType === 3) return document.createTextNode(this.data);
      if (this._uid) {
         const holder = document.createElement('div');
         holder.innerHTML = deep ? this.outerHTML : this.outerHTML.replace(/>[\s\S]*$/, '>' + '</' + this.localName + '>');
         const c = holder.firstElementChild;
         if (c) holder.removeChild(c);
         return c;
      }
      const c = document.createElement(this.localName || 'div');
      if (this._attrs) for (const [k, v] of this._attrs) c.setAttribute(k, v);
      if (deep) for (const k of this.childNodes) c.appendChild(k.cloneNode(true));
      if (this._text !== undefined) c._text = this._text;
      return c;
   }
}
Node.ELEMENT_NODE = 1; Node.TEXT_NODE = 3; Node.COMMENT_NODE = 8; Node.DOCUMENT_NODE = 9; Node.DOCUMENT_FRAGMENT_NODE = 11;
Node.DOCUMENT_POSITION_PRECEDING = 2; Node.DOCUMENT_POSITION_FOLLOWING = 4;
Node.DOCUMENT_POSITION_CONTAINS = 8; Node.DOCUMENT_POSITION_CONTAINED_BY = 16;

function detach(c) {
   if (c._vparent) {
      const k = c._vparent._vkids, i = k ? k.indexOf(c) : -1;
      if (i >= 0) k.splice(i, 1);
      c._vparent = null;
   }
   if (c._host) {                     /* text folded into its parent */
      L.setText(c._host._uid, '');
      c._host = null;
   }
   if (c._uid) L.remove(c._uid);
}

/* A node just joined the document: run what a browser runs on insertion. */
function connected(n) {
   if (!n.isConnected) return;
   if (n.localName === 'script' && !n._uid && !n._started) {
      n._started = true;
      const type = (n.getAttribute('type') || '').toLowerCase();
      if (type && !/^(text|application)\/(x-)?(java|ecma)script$/.test(type) && type !== 'module') return;
      const src = n.getAttribute('src');
      if (src) {
         setTimeout(() => {
            const prev = document._current;
            document._current = n;
            const ok = L.evalURL(src, type === 'module');
            document._current = prev;
            n.dispatchEvent(new Event(ok ? 'load' : 'error'));
         }, 0);
      } else if (n.textContent) {
         const prev = document._current;
         document._current = n;
         try { (0, eval)(n.textContent); } catch (e) { reportError(e); }
         document._current = prev;
      }
   } else if (n.localName === 'style' && !n._uid) {
      L.css(n.textContent || '');
   } else if (n.localName === 'link' && !n._uid && /stylesheet/i.test(n.getAttribute('rel') || '')) {
      const href = n.getAttribute('href');
      setTimeout(() => {
         const r = href ? L.http('GET', href, '', null, false) : { status: 0 };
         if (r.status >= 200 && r.status < 300) L.css(r.body);
         n.dispatchEvent(new Event(r.status >= 200 && r.status < 300 ? 'load' : 'error'));
      }, 0);
   }
   if (n._vkids) for (const k of n._vkids) connected(k);
}

class CharacterData extends Node {
   get length() { return this.data.length; }
   remove() { if (this.parentNode) this.parentNode.removeChild(this); }
}

class Text extends CharacterData {
   constructor(data = '') { super(); hide(this, '_data', String(data)); }
   get nodeType() { return 3; }
   get nodeName() { return '#text'; }
   get data() { return this._uid ? L.text(this._uid) : this._data; }
   set data(v) {
      this._data = String(v);
      if (this._uid) L.setText(this._uid, this._data);
      else if (this._host) L.setText(this._host._uid, this._data);
   }
   get nodeValue() { return this.data; } set nodeValue(v) { this.data = v; }
   get textContent() { return this.data; } set textContent(v) { this.data = v; }
   get wholeText() { return this.data; }
   get parentNode() {
      if (this._host) return this._host;
      if (this._vparent) return this._vparent;
      return this._uid ? W(L.parent(this._uid)) : null;
   }
   get childNodes() { return list([]); }
   /* Rendered as an inline <span> once it has element siblings. */
   _materialize() {
      if (this._uid) return;
      const uid = L.createText(this._data);
      hide(this, '_uid', uid);
      wrappers.set(uid, this);
   }
   splitText(o) { const t = new Text(this._data.slice(o)); this.data = this._data.slice(0, o); if (this.parentNode) this.parentNode.insertBefore(t, this.nextSibling); return t; }
}
class Comment extends CharacterData {
   constructor(data = '') { super(); this.data = String(data); }
   get nodeType() { return 8; }
   get nodeName() { return '#comment'; }
   get parentNode() { return this._vparent || null; }
   get childNodes() { return list([]); }
   get textContent() { return this.data; }
}

class DOMTokenList {
   constructor(el, attr) { hide(this, '_el', el); hide(this, '_attr', attr); }
   _get() { return (this._el.getAttribute(this._attr) || '').split(/\s+/).filter(Boolean); }
   _set(a) { this._el.setAttribute(this._attr, a.join(' ')); }
   get length() { return this._get().length; }
   get value() { return this._el.getAttribute(this._attr) || ''; }
   set value(v) { this._el.setAttribute(this._attr, v); }
   item(i) { return this._get()[i] || null; }
   contains(c) { return this._get().includes(c); }
   add(...cs) { const a = this._get(); let ch = false; for (const c of cs) if (!a.includes(c)) { a.push(c); ch = true; } if (ch) this._set(a); }
   remove(...cs) { const a = this._get(), b = a.filter(c => !cs.includes(c)); if (b.length !== a.length) this._set(b); }
   toggle(c, force) {
      const has = this.contains(c);
      if (force === true || (force === undefined && !has)) { if (!has) this.add(c); return true; }
      if (has) this.remove(c);
      return false;
   }
   replace(a, b) { const l = this._get(), i = l.indexOf(a); if (i < 0) return false; l[i] = b; this._set(l); return true; }
   forEach(f, t) { this._get().forEach(f, t); }
   entries() { return this._get().entries(); }
   keys() { return this._get().keys(); }
   values() { return this._get().values(); }
   [Symbol.iterator]() { return this._get()[Symbol.iterator](); }
   toString() { return this.value; }
}

const camel2kebab = s => s.startsWith('--') ? s : s.replace(/[A-Z]/g, m => '-' + m.toLowerCase()).replace(/^(webkit|moz|ms)-/, '-$1-');
function parseStyle(s) {
   const m = new Map();
   for (const decl of (s || '').split(';')) {
      const i = decl.indexOf(':');
      if (i > 0) m.set(decl.slice(0, i).trim().toLowerCase(), decl.slice(i + 1).trim());
   }
   return m;
}
function styleProxy(el) {
   const target = {
      getPropertyValue(p) { return parseStyle(el.getAttribute('style')).get(p) || ''; },
      setProperty(p, v, prio) {
         const m = parseStyle(el.getAttribute('style'));
         if (v === null || v === undefined || v === '') m.delete(p);
         else m.set(p, String(v) + (prio ? ' !' + prio : ''));
         el.setAttribute('style', [...m].map(([k, x]) => k + ':' + x).join(';'));
      },
      removeProperty(p) { const v = this.getPropertyValue(p); this.setProperty(p, ''); return v; },
      item(i) { return [...parseStyle(el.getAttribute('style')).keys()][i] || ''; },
      get length() { return parseStyle(el.getAttribute('style')).size; },
      get cssText() { return el.getAttribute('style') || ''; },
      set cssText(v) { el.setAttribute('style', v); },
   };
   return new Proxy(target, {
      get(t, k) {
         if (k in t) return typeof t[k] === 'function' ? t[k].bind(t) : t[k];
         if (typeof k !== 'string') return undefined;
         if (k === 'cssFloat') k = 'float';
         return t.getPropertyValue(camel2kebab(k));
      },
      set(t, k, v) {
         if (k === 'cssText') { t.cssText = v; return true; }
         if (typeof k === 'string') t.setProperty(camel2kebab(k === 'cssFloat' ? 'float' : k), v);
         return true;
      },
   });
}
function datasetProxy(el) {
   const key = k => 'data-' + camel2kebab(k);
   return new Proxy({}, {
      get(t, k) { if (typeof k !== 'string') return undefined; const v = el.getAttribute(key(k)); return v === null ? undefined : v; },
      set(t, k, v) { el.setAttribute(key(k), String(v)); return true; },
      deleteProperty(t, k) { el.removeAttribute(key(k)); return true; },
      has(t, k) { return el.hasAttribute(key(k)); },
      ownKeys() { return el.getAttributeNames().filter(n => n.startsWith('data-')).map(n => n.slice(5).replace(/-([a-z])/g, (m, c) => c.toUpperCase())); },
      getOwnPropertyDescriptor(t, k) { const v = el.getAttribute(key(k)); return v === null ? undefined : { value: v, enumerable: true, configurable: true }; },
   });
}

class DOMRect {
   constructor(x = 0, y = 0, w = 0, h = 0) { this.x = x; this.y = y; this.width = w; this.height = h; }
   get left() { return this.x; } get top() { return this.y; }
   get right() { return this.x + this.width; } get bottom() { return this.y + this.height; }
   toJSON() { return { x: this.x, y: this.y, width: this.width, height: this.height, top: this.top, left: this.left, right: this.right, bottom: this.bottom }; }
}

const VOID = new Set(['area', 'base', 'br', 'col', 'embed', 'hr', 'img', 'input', 'link', 'meta', 'param', 'source', 'track', 'wbr']);
const escText = s => String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
const escAttr = s => String(s).replace(/&/g, '&amp;').replace(/"/g, '&quot;');

class Element extends Node {
   get nodeType() { return 1; }
   get localName() { return this._uid ? L.tag(this._uid).toLowerCase() : this._tag; }
   get tagName() { return this.localName.toUpperCase(); }
   get nodeName() { return this.tagName; }
   get namespaceURI() { return 'http://www.w3.org/1999/xhtml'; }
   get parentNode() {
      if (this._vparent) return this._vparent;
      if (!this._uid) return null;
      const p = L.parent(this._uid);
      if (p) return W(p);
      if (this.localName === 'body' && L.connected(this._uid)) return document.documentElement;
      return null;
   }
   get childNodes() {
      const out = this._uid ? L.children(this._uid).map(W) : [];
      if (this._uid && !out.length && !this._vkids) {
         const t = L.text(this._uid);
         if (t && this.localName !== 'input' && this.localName !== 'textarea') return list([ownText(this, t)]);
      }
      if (this._vkids) out.push(...this._vkids);
      return list(out);
   }
   get children() { return list(this.childNodes.filter(n => n.nodeType === 1)); }
   get childElementCount() { return this.children.length; }
   get firstElementChild() { return this.children[0] || null; }
   get lastElementChild() { const c = this.children; return c[c.length - 1] || null; }
   get nextElementSibling() { const p = this.parentNode; if (!p) return null; const c = p.children; return c[c.indexOf(this) + 1] || null; }
   get previousElementSibling() { const p = this.parentNode; if (!p) return null; const c = p.children; const i = c.indexOf(this); return i > 0 ? c[i - 1] : null; }

   getAttribute(n) {
      n = String(n).toLowerCase();
      if (this._uid) return L.getAttr(this._uid, n);
      return this._attrs && this._attrs.has(n) ? this._attrs.get(n) : null;
   }
   setAttribute(n, v) {
      n = String(n).toLowerCase(); v = String(v);
      if (this._uid) L.setAttr(this._uid, n, v);
      else { if (!this._attrs) hide(this, '_attrs', new Map()); this._attrs.set(n, v); }
   }
   removeAttribute(n) {
      n = String(n).toLowerCase();
      if (this._uid) L.removeAttr(this._uid, n); else if (this._attrs) this._attrs.delete(n);
   }
   hasAttribute(n) { return this.getAttribute(n) !== null; }
   toggleAttribute(n, f) { const h = this.hasAttribute(n); if (f === true || (f === undefined && !h)) { this.setAttribute(n, ''); return true; } this.removeAttribute(n); return false; }
   getAttributeNames() { return this._uid ? L.attrNames(this._uid) : this._attrs ? [...this._attrs.keys()] : []; }
   hasAttributes() { return this.getAttributeNames().length > 0; }
   get attributes() {
      const a = this.getAttributeNames().map(n => ({ name: n, localName: n, value: this.getAttribute(n), nodeName: n }));
      a.getNamedItem = n => a.find(x => x.name === n) || null;
      a.item = i => a[i] || null;
      return a;
   }
   setAttributeNS(ns, n, v) { this.setAttribute(n, v); }
   getAttributeNS(ns, n) { return this.getAttribute(n); }
   removeAttributeNS(ns, n) { this.removeAttribute(n); }

   get id() { return this.getAttribute('id') || ''; } set id(v) { this.setAttribute('id', v); }
   get className() { return this.getAttribute('class') || ''; } set className(v) { this.setAttribute('class', v); }
   get classList() { return new DOMTokenList(this, 'class'); }
   get slot() { return ''; }

   get textContent() {
      if (this._uid) return L.text(this._uid) + (this._vkids ? this._vkids.map(k => k.textContent).join('') : '');
      if (this._text !== undefined) return this._text;
      return (this._vkids || []).map(k => k.textContent || '').join('');
   }
   set textContent(v) {
      v = v === null || v === undefined ? '' : String(v);
      if (this._uid) { this._vkids = null; L.setText(this._uid, v); }
      else { this._vkids = null; this._text = v; if (this.localName === 'style' && this.isConnected) L.css(v); }
   }
   get innerText() { return this.textContent; } set innerText(v) { this.textContent = v; }
   get outerText() { return this.textContent; }
   get innerHTML() {
      if (this._uid) return L.html(this._uid, false);
      if (this._text !== undefined) return this._text;
      return (this._vkids || []).map(k => k.nodeType === 3 ? escText(k.data) : k.outerHTML || '').join('');
   }
   set innerHTML(v) {
      v = v === null || v === undefined ? '' : String(v);
      if (this._uid) { this._vkids = null; L.setHTML(this._uid, v); }
      else if (/^(script|style|title|textarea)$/.test(this.localName)) this._text = v;
      else {
         /* Parse through a rendered holder, then move the nodes here. */
         const holder = document.createElement('div');
         L.setHTML(holder._uid, v);
         this._vkids = null;
         for (const k of holder.childNodes) this.appendChild(k);
      }
   }
   get outerHTML() {
      if (this._uid) return L.html(this._uid, true);
      const attrs = this.getAttributeNames().map(n => ' ' + n + '="' + escAttr(this.getAttribute(n)) + '"').join('');
      if (VOID.has(this.localName)) return '<' + this.localName + attrs + '>';
      return '<' + this.localName + attrs + '>' + this.innerHTML + '</' + this.localName + '>';
   }
   set outerHTML(v) {
      const p = this.parentNode;
      if (!p) return;
      const holder = document.createElement('div');
      holder.innerHTML = v;
      for (const k of holder.childNodes) p.insertBefore(k, this);
      p.removeChild(this);
   }
   insertAdjacentHTML(pos, html) {
      const holder = document.createElement('div');
      holder.innerHTML = html;
      const nodes = holder.childNodes;
      insertAdjacent(this, pos, nodes);
   }
   insertAdjacentElement(pos, el) { insertAdjacent(this, pos, [el]); return el; }
   insertAdjacentText(pos, t) { insertAdjacent(this, pos, [document.createTextNode(t)]); }
   remove() { const p = this.parentNode; if (p) p.removeChild(this); else detach(this); }
   before(...ns) { const p = this.parentNode; if (p) for (const n of ns) p.insertBefore(typeof n === 'string' ? document.createTextNode(n) : n, this); }
   after(...ns) { const p = this.parentNode; if (!p) return; const r = this.nextSibling; for (const n of ns) p.insertBefore(typeof n === 'string' ? document.createTextNode(n) : n, r); }
   replaceWith(...ns) { const p = this.parentNode; if (!p) return; this.before(...ns); p.removeChild(this); }

   querySelector(s) { return this._uid ? W(L.query(this._uid, String(s), false)) : vquery(this, s, false)[0] || null; }
   querySelectorAll(s) { return list(this._uid ? L.query(this._uid, String(s), true).map(W) : vquery(this, s, true)); }
   getElementsByClassName(c) { return this.querySelectorAll(String(c).trim().split(/\s+/).map(x => '.' + CSS.escape(x)).join('')); }
   getElementsByTagName(t) { return this.querySelectorAll(t === '*' ? '*' : CSS.escape(t)); }
   matches(s) {
      if (this._uid) return L.matches(this._uid, String(s));
      return String(s).split(',').some(x => x.trim().toLowerCase() === this.localName);
   }
   webkitMatchesSelector(s) { return this.matches(s); }
   closest(s) { for (let n = this; n && n.nodeType === 1; n = n.parentNode) if (n.matches(s)) return n; return null; }

   getBoundingClientRect() {
      if (!this._uid) return new DOMRect();
      const r = L.rect(this._uid);
      return new DOMRect(r[0], r[1], r[2], r[3]);
   }
   getClientRects() { const r = this.getBoundingClientRect(); return r.width || r.height ? [r] : []; }
   get offsetWidth() { return Math.round(this.getBoundingClientRect().width); }
   get offsetHeight() { return Math.round(this.getBoundingClientRect().height); }
   get offsetLeft() { return Math.round(this.getBoundingClientRect().x); }
   get offsetTop() { return Math.round(this.getBoundingClientRect().y); }
   get offsetParent() { return this.isConnected ? document.body : null; }
   get clientWidth() { return this.offsetWidth; }
   get clientHeight() { return this.offsetHeight; }
   get clientLeft() { return 0; } get clientTop() { return 0; }
   get scrollTop() { return this._uid ? L.scroll(this._uid)[0] : 0; }
   set scrollTop(v) { if (this._uid) L.scroll(this._uid, +v || 0); }
   get scrollLeft() { return this._uid ? L.scroll(this._uid)[1] : 0; }
   set scrollLeft(v) { if (this._uid) L.scroll(this._uid, undefined, +v || 0); }
   get scrollHeight() { return this._uid ? Math.max(L.scroll(this._uid)[2], this.clientHeight) : 0; }
   get scrollWidth() { return this._uid ? Math.max(L.scroll(this._uid)[3], this.clientWidth) : 0; }
   scrollTo(x, y) { if (typeof x === 'object') { y = x.top; x = x.left; } if (this._uid) L.scroll(this._uid, y, x); }
   scroll(x, y) { this.scrollTo(x, y); }
   scrollBy(x, y) { if (typeof x === 'object') { y = x.top; x = x.left; } this.scrollTo((this.scrollLeft + (x || 0)), (this.scrollTop + (y || 0))); }
   scrollIntoView() {}
   focus() { if (this._uid) L.focus(this._uid); syncFocus(); }
   blur() { if (this._uid && L.focus() === this._uid) L.focus(0); syncFocus(); }
   click() { const e = new MouseEvent('click', { bubbles: true, cancelable: true }); if (dispatch(this, e)) activate(this); }
   get style() { return styleProxy(this); }
   set style(v) { this.setAttribute('style', v); }
   get dataset() { return datasetProxy(this); }
   get hidden() { return this.hasAttribute('hidden'); } set hidden(v) { this.toggleAttribute('hidden', !!v); }
   get title() { return this.getAttribute('title') || ''; } set title(v) { this.setAttribute('title', v); }
   get lang() { return this.getAttribute('lang') || ''; }
   get dir() { return this.getAttribute('dir') || ''; }
   get tabIndex() { const t = this.getAttribute('tabindex'); return t === null ? -1 : +t; } set tabIndex(v) { this.setAttribute('tabindex', v); }
   get contentEditable() { return this.getAttribute('contenteditable') || 'inherit'; }
   get isContentEditable() { return false; }
   get shadowRoot() { return null; }
   attachShadow() { return this; }
   animate() { const a = { finished: Promise.resolve(), cancel() {}, finish() {}, play() {}, pause() {}, onfinish: null }; setTimeout(() => a.onfinish && a.onfinish(), 0); return a; }
   getAnimations() { return []; }
   requestFullscreen() { return Promise.resolve(); }
   setPointerCapture() {} releasePointerCapture() {} hasPointerCapture() { return false; }
}
function ownText(el, t) {
   const n = new Text(t);
   hide(n, '_host', el);
   return n;
}
function insertAdjacent(el, pos, nodes) {
   nodes = Array.from(nodes);
   switch (String(pos).toLowerCase()) {
   case 'beforebegin': for (const n of nodes) el.parentNode && el.parentNode.insertBefore(n, el); break;
   case 'afterbegin': { const f = el.firstChild; for (const n of nodes) el.insertBefore(n, f); break; }
   case 'beforeend': for (const n of nodes) el.appendChild(n); break;
   case 'afterend': { const r = el.nextSibling; for (const n of nodes) el.parentNode && el.parentNode.insertBefore(n, r); break; }
   }
}
function vquery(root, sel, all) {
   /* Virtual subtrees: type selectors and [attr] only, plus rendered
    * descendants. */
   const out = [];
   const walk = n => {
      for (const k of n.childNodes) {
         if (k.nodeType !== 1) continue;
         if (vmatch(k, sel)) out.push(k);
         if (k._uid) out.push(...L.query(k._uid, String(sel), true).map(W));
         else walk(k);
      }
   };
   walk(root);
   return all ? out : out.slice(0, 1);
}
function vmatch(el, sel) {
   if (el._uid) return L.matches(el._uid, String(sel));
   return String(sel).split(',').some(s => {
      const m = /^\s*([a-z0-9*-]*)((?:\[[^\]]+\])*)\s*$/i.exec(s);
      if (!m) return false;
      if (m[1] && m[1] !== '*' && m[1].toLowerCase() !== el.localName) return false;
      for (const a of m[2].match(/\[[^\]]+\]/g) || []) {
         const am = /^\[\s*([^\]~|^$*=\s]+)\s*(?:([~|^$*]?=)\s*["']?([^"'\]]*)["']?)?\s*\]$/.exec(a);
         if (!am) return false;
         const v = el.getAttribute(am[1]);
         if (v === null) return false;
         if (am[2] === '=' && v !== am[3]) return false;
         if (am[2] === '^=' && !v.startsWith(am[3])) return false;
         if (am[2] === '$=' && !v.endsWith(am[3])) return false;
         if (am[2] === '*=' && !v.includes(am[3])) return false;
      }
      return true;
   });
}

class HTMLElement extends Element {}
const reflect = (C, props) => {
   for (const p of props) {
      const [name, attr, kind] = Array.isArray(p) ? p : [p, p.toLowerCase(), 's'];
      Object.defineProperty(C.prototype, name, {
         configurable: true,
         get() {
            if (kind === 'b') return this.hasAttribute(attr);
            if (kind === 'u') { const v = this.getAttribute(attr); return v === null ? '' : L.resolve(v) || v; }
            if (kind === 'n') { const v = this.getAttribute(attr); return v === null ? 0 : +v; }
            return this.getAttribute(attr) || '';
         },
         set(v) { if (kind === 'b') this.toggleAttribute(attr, !!v); else this.setAttribute(attr, v); },
      });
   }
};
class HTMLInputElement extends HTMLElement {
   get value() { return this._uid ? L.value(this._uid) : this.getAttribute('value') || ''; }
   set value(v) { if (this._uid) L.value(this._uid, String(v)); else this.setAttribute('value', v); }
   get checked() { return this._checked !== undefined ? this._checked : this.hasAttribute('checked'); }
   set checked(v) { hide(this, '_checked', !!v); }
   get defaultValue() { return this.getAttribute('value') || ''; }
   get form() { return this.closest('form'); }
   select() { this.focus(); }
   setSelectionRange() {}
   get selectionStart() { return this.value.length; } get selectionEnd() { return this.value.length; }
   checkValidity() { return true; } reportValidity() { return true; } setCustomValidity() {}
   get validity() { return { valid: true }; }
   get files() { return []; }
}
reflect(HTMLInputElement, ['type', 'name', 'placeholder', ['disabled', 'disabled', 'b'], ['readOnly', 'readonly', 'b'],
                           ['required', 'required', 'b'], 'autocomplete', ['maxLength', 'maxlength', 'n'], 'min', 'max', 'step', 'pattern']);
class HTMLTextAreaElement extends HTMLInputElement {}
class HTMLSelectElement extends HTMLElement {
   get options() { return this.querySelectorAll('option'); }
   get selectedIndex() { const o = this.options; const i = o.findIndex(x => x.selected); return i < 0 ? (o.length ? 0 : -1) : i; }
   set selectedIndex(i) { this.options.forEach((o, k) => { o.selected = k === i; }); }
   get value() { const o = this.options[this.selectedIndex]; return o ? o.value : ''; }
   set value(v) { this.options.forEach(o => { o.selected = o.value === String(v); }); }
   get form() { return this.closest('form'); }
}
reflect(HTMLSelectElement, ['name', ['disabled', 'disabled', 'b'], ['multiple', 'multiple', 'b']]);
class HTMLOptionElement extends HTMLElement {
   get value() { const v = this.getAttribute('value'); return v === null ? this.textContent : v; }
   set value(v) { this.setAttribute('value', v); }
   get text() { return this.textContent; }
   get selected() { return this.hasAttribute('selected'); } set selected(v) { this.toggleAttribute('selected', !!v); }
}
class HTMLButtonElement extends HTMLElement { get form() { return this.closest('form'); } }
reflect(HTMLButtonElement, ['type', 'name', 'value', ['disabled', 'disabled', 'b']]);
class HTMLAnchorElement extends HTMLElement {
   toString() { return this.href; }
}
reflect(HTMLAnchorElement, [['href', 'href', 'u'], 'target', 'rel', 'download']);
/* HTMLHyperlinkElementUtils: the parts of href, read and written through URL. */
for (const k of ['protocol', 'host', 'hostname', 'port', 'pathname', 'search', 'hash', 'origin', 'username', 'password']) {
   Object.defineProperty(HTMLAnchorElement.prototype, k, {
      configurable: true,
      get() { try { return new URL(this.href)[k]; } catch (e) { return ''; } },
      set(v) {
         if (k === 'origin') return;
         try { const u = new URL(this.href); u[k] = v; this.setAttribute('href', u.href); } catch (e) {}
      },
   });
}
class HTMLImageElement extends HTMLElement {
   get complete() { return true; }
   get naturalWidth() { return this.offsetWidth; } get naturalHeight() { return this.offsetHeight; }
   decode() { return Promise.resolve(); }
}
reflect(HTMLImageElement, [['src', 'src', 'u'], 'alt', ['width', 'width', 'n'], ['height', 'height', 'n'], 'srcset', 'loading', 'crossOrigin']);
Object.defineProperty(HTMLImageElement.prototype, 'src', {
   configurable: true,
   get() { const v = this.getAttribute('src'); return v === null ? '' : L.resolve(v) || v; },
   set(v) { this.setAttribute('src', v); setTimeout(() => this.dispatchEvent(new Event('load')), 0); },
});
class HTMLFormElement extends HTMLElement {
   get elements() { return this.querySelectorAll('input,select,textarea,button'); }
   submit() { submitForm(this); }
   requestSubmit() { if (this.dispatchEvent(new Event('submit', { bubbles: true, cancelable: true }))) submitForm(this); }
   reset() {}
   checkValidity() { return true; } reportValidity() { return true; }
}
reflect(HTMLFormElement, [['action', 'action', 'u'], 'method', 'target', 'enctype', 'name']);
class HTMLScriptElement extends HTMLElement {
   get text() { return this.textContent; } set text(v) { this.textContent = v; }
}
reflect(HTMLScriptElement, [['src', 'src', 'u'], 'type', ['async', 'async', 'b'], ['defer', 'defer', 'b'], 'crossOrigin', 'integrity', 'charset', 'nonce']);
class HTMLStyleElement extends HTMLElement { get sheet() { return { cssRules: [], insertRule(r) { L.css(r); return 0; } }; } }
class HTMLLinkElement extends HTMLElement {}
reflect(HTMLLinkElement, [['href', 'href', 'u'], 'rel', 'as', 'type', 'media', 'crossOrigin']);
class HTMLMetaElement extends HTMLElement {}
reflect(HTMLMetaElement, ['name', 'content', ['httpEquiv', 'http-equiv', 's']]);
class HTMLCanvasElement extends HTMLElement {
   getContext() { return null; }
   toDataURL() { return 'data:,'; }
   toBlob(cb) { setTimeout(() => cb(null), 0); }
}
reflect(HTMLCanvasElement, [['width', 'width', 'n'], ['height', 'height', 'n']]);
class HTMLMediaElement extends HTMLElement {
   play() { return Promise.reject(new DOMException('media playback is not available', 'NotSupportedError')); }
   pause() {} load() {} canPlayType() { return ''; }
   get paused() { return true; } get duration() { return NaN; } get currentTime() { return 0; } set currentTime(v) {}
   get muted() { return true; } set muted(v) {} get volume() { return 0; } set volume(v) {}
}
class HTMLVideoElement extends HTMLMediaElement {}
class HTMLAudioElement extends HTMLMediaElement {}
class HTMLIFrameElement extends HTMLElement { get contentWindow() { return null; } get contentDocument() { return null; } }
reflect(HTMLIFrameElement, [['src', 'src', 'u'], 'name', 'allow']);
class HTMLTemplateElement extends HTMLElement {
   get content() {
      if (!this._content) {
         const f = document.createDocumentFragment();
         if (this._text) { const h = document.createElement('div'); h.innerHTML = this._text; for (const k of h.childNodes) f.appendChild(k); }
         hide(this, '_content', f);
      }
      return this._content;
   }
}
class HTMLLabelElement extends HTMLElement {
   get control() { const f = this.getAttribute('for'); return f ? document.getElementById(f) : this.querySelector('input,select,textarea'); }
}
reflect(HTMLLabelElement, [['htmlFor', 'for', 's']]);
class HTMLBodyElement extends HTMLElement {}
class HTMLHeadElement extends HTMLElement {}
class HTMLHtmlElement extends HTMLElement {}
class HTMLDivElement extends HTMLElement {}
class HTMLSpanElement extends HTMLElement {}
class HTMLParagraphElement extends HTMLElement {}
class HTMLUListElement extends HTMLElement {}
class HTMLLIElement extends HTMLElement {}
class HTMLTitleElement extends HTMLElement {}
class SVGElement extends Element {}

const TAGS = {
   input: HTMLInputElement, textarea: HTMLTextAreaElement, select: HTMLSelectElement, option: HTMLOptionElement,
   button: HTMLButtonElement, a: HTMLAnchorElement, img: HTMLImageElement, form: HTMLFormElement,
   script: HTMLScriptElement, style: HTMLStyleElement, link: HTMLLinkElement, meta: HTMLMetaElement,
   canvas: HTMLCanvasElement, video: HTMLVideoElement, audio: HTMLAudioElement, iframe: HTMLIFrameElement,
   template: HTMLTemplateElement, label: HTMLLabelElement, body: HTMLBodyElement, head: HTMLHeadElement,
   html: HTMLHtmlElement, div: HTMLDivElement, span: HTMLSpanElement, p: HTMLParagraphElement,
   ul: HTMLUListElement, li: HTMLLIElement, title: HTMLTitleElement, svg: SVGElement,
};

function virtual(tag) {
   const C = TAGS[tag] || HTMLElement;
   const el = Object.create(C.prototype);
   hide(el, '_tag', tag);
   hide(el, '_uid', 0);
   return el;
}

class DocumentFragment extends Node {
   get nodeType() { return 11; }
   get nodeName() { return '#document-fragment'; }
   get parentNode() { return null; }
   get childNodes() { return list(this._vkids || []); }
   get children() { return list(this.childNodes.filter(n => n.nodeType === 1)); }
   get firstElementChild() { return this.children[0] || null; }
   get textContent() { return this.childNodes.map(k => k.textContent).join(''); }
   querySelector(s) { return vquery(this, s, false)[0] || null; }
   querySelectorAll(s) { return list(vquery(this, s, true)); }
   getElementById(id) { return this.querySelector('#' + CSS.escape(id)); }
}

/* --------------------------------------------------------------- document */

class Document extends Node {
   constructor() {
      super();
      hide(this, '_ready', 'loading');
      hide(this, '_html', virtual('html'));
      hide(this, '_head', virtual('head'));
      hide(this._html, '_vkids', [this._head]);
      hide(this._head, '_vparent', this._html);
      hide(this._html, '_vparent', this);
   }
   get nodeType() { return 9; }
   get nodeName() { return '#document'; }
   get parentNode() { return null; }
   get documentElement() { return this._html; }
   get head() { return this._head; }
   get body() { return W(L.body()); }
   set body(v) {}
   get childNodes() { return list([this._html]); }
   get children() { return this.childNodes; }
   get firstElementChild() { return this._html; }
   get defaultView() { return g; }
   get readyState() { return this._ready; }
   get title() { return L.title(); } set title(v) { L.title(String(v)); }
   get URL() { return L.url(); }
   get documentURI() { return L.url(); }
   get baseURI() { return L.url(); }
   get domain() { return location.hostname; }
   get referrer() { return ''; }
   get cookie() { return L.cookie(); } set cookie(v) { L.cookie(String(v)); }
   get characterSet() { return 'UTF-8'; } get charset() { return 'UTF-8'; } get inputEncoding() { return 'UTF-8'; }
   get contentType() { return 'text/html'; }
   get compatMode() { return 'CSS1Compat'; }
   get visibilityState() { return 'visible'; } get hidden() { return false; }
   get activeElement() { return W(L.focus()) || this.body; }
   get currentScript() { return this._current || null; }
   get scripts() { return this.querySelectorAll('script'); }
   get forms() { return this.querySelectorAll('form'); }
   get images() { return this.querySelectorAll('img'); }
   get links() { return this.querySelectorAll('a[href]'); }
   get fonts() { return { ready: Promise.resolve(), status: 'loaded', addEventListener() {}, check() { return true; }, load() { return Promise.resolve([]); }, add() {} }; }
   get location() { return location; } set location(v) { location.href = v; }
   get implementation() { return { createHTMLDocument: () => document, hasFeature: () => true }; }
   get styleSheets() { return []; }
   hasFocus() { return true; }
   getElementById(id) { return W(L.byId(String(id))) || vquery(this._head, '#' + id, false)[0] || null; }
   getElementsByName(n) { return this.querySelectorAll('[name="' + String(n).replace(/"/g, '\\"') + '"]'); }
   querySelector(s) { return W(L.query(0, String(s), false)) || vquery(this._html, s, false)[0] || null; }
   querySelectorAll(s) {
      const r = L.query(0, String(s), true).map(W);
      for (const v of vquery(this._head, s, true)) if (!r.includes(v)) r.push(v);
      return list(r);
   }
   getElementsByClassName(c) { return this.querySelectorAll(String(c).trim().split(/\s+/).map(x => '.' + CSS.escape(x)).join('')); }
   getElementsByTagName(t) {
      t = String(t).toLowerCase();
      if (t === 'head') return list([this._head]);
      if (t === 'html') return list([this._html]);
      return this.querySelectorAll(t === '*' ? '*' : t);
   }
   createElement(tag) {
      tag = String(tag).toLowerCase();
      const uid = L.create(tag, null);
      return uid ? W(uid) : virtual(tag);
   }
   createElementNS(ns, tag) { return this.createElement(String(tag).replace(/^.*:/, '')); }
   createTextNode(t) { return new Text(t); }
   createComment(t) { return new Comment(t); }
   createDocumentFragment() { return new DocumentFragment(); }
   createEvent(kind) { return new (g[kind] || g[kind.replace(/s$/, '')] || Event)(''); }
   createRange() {
      return { selectNodeContents() {}, setStart() {}, setEnd() {}, collapse() {}, getBoundingClientRect: () => new DOMRect(),
               createContextualFragment: html => { const t = this.createElement('template'); t.innerHTML = html; return t.content; } };
   }
   createTreeWalker(root) {
      const nodes = [root, ...Array.from(root.querySelectorAll ? root.querySelectorAll('*') : [])];
      let i = 0;
      return { currentNode: root, nextNode() { i++; return (this.currentNode = nodes[i] || null); } };
   }
   importNode(n, deep) { return n.cloneNode(deep); }
   adoptNode(n) { return n; }
   execCommand() { return false; }
   getSelection() { return g.getSelection(); }
   elementFromPoint(x, y) { return this.body; }
   open() { return this; } close() {}
   write(...s) {
      /* After load the document is replaced; during it, content is appended. */
      const b = this.body;
      if (b) b.insertAdjacentHTML('beforeend', s.join(''));
   }
   writeln(...s) { this.write(...s, '\n'); }
}
function syncFocus() {}

const document = new Document();
g.document = document;

/* ---------------------------------------------------------------- window */

g.window = g; g.self = g; g.top = g; g.parent = g; g.frames = g; g.opener = null;
g.name = ''; g.closed = false; g.length = 0; g.frameElement = null;
for (const k of ['addEventListener', 'removeEventListener', 'dispatchEvent'])
   g[k] = EventTarget.prototype[k];
Object.defineProperties(g, {
   innerWidth: { get: () => L.viewport()[0], configurable: true },
   innerHeight: { get: () => L.viewport()[1], configurable: true },
   outerWidth: { get: () => L.viewport()[0], configurable: true },
   outerHeight: { get: () => L.viewport()[1], configurable: true },
   scrollX: { get: () => 0, configurable: true }, scrollY: { get: () => 0, configurable: true },
   pageXOffset: { get: () => 0, configurable: true }, pageYOffset: { get: () => 0, configurable: true },
   screenX: { value: 0, configurable: true }, screenY: { value: 0, configurable: true },
   isSecureContext: { get: () => /^(https:|file:|about:)/.test(L.url()) || /^http:\/\/(localhost|127\.)/.test(L.url()), configurable: true },
   origin: { get: () => location.origin, configurable: true },
});
Object.defineProperty(g, 'devicePixelRatio', { get: () => L.viewport()[2] || 1, configurable: true });
g.screen = {
   get width() { return L.viewport()[0]; }, get height() { return L.viewport()[1]; },
   get availWidth() { return L.viewport()[0]; }, get availHeight() { return L.viewport()[1]; },
   colorDepth: 24, pixelDepth: 24,
   orientation: { get type() { const v = L.viewport(); return v[0] >= v[1] ? 'landscape-primary' : 'portrait-primary'; }, angle: 0, addEventListener() {}, removeEventListener() {}, lock: () => Promise.resolve() },
};
g.scrollTo = g.scroll = (x, y) => { const b = document.body; if (b) b.scrollTo(x, y); };
g.scrollBy = (x, y) => { const b = document.body; if (b) b.scrollBy(x, y); };
g.alert = m => L.log('info', 'alert: ' + m);
g.confirm = m => { L.log('info', 'confirm: ' + m); return true; };
g.prompt = (m, d) => { L.log('info', 'prompt: ' + m); return d === undefined ? null : d; };
g.print = () => {};
g.focus = () => {}; g.blur = () => {}; g.stop = () => {};
g.close = () => { g.closed = true; };
g.open = (u) => { if (u) location.assign(u); return null; };
g.postMessage = (data, origin) => setTimeout(() => dispatch(g, new MessageEvent('message', { data, origin: location.origin, source: g })), 0);
g.getSelection = () => ({ rangeCount: 0, toString: () => '', removeAllRanges() {}, addRange() {}, getRangeAt() { return document.createRange(); }, collapse() {} });
g.getComputedStyle = (el) => {
   const decl = el && el.style ? el.style : styleProxy(document.createElement('div'));
   return new Proxy(decl, {
      get(t, k) {
         const v = t[k];
         if (v !== '' || typeof k !== 'string') return v;
         if (k === 'display') return el && el.localName && /^(span|a|b|i|em|strong|img|label|input|button|select)$/.test(el.localName) ? 'inline' : 'block';
         if (k === 'visibility') return 'visible';
         if (k === 'opacity') return '1';
         if (k === 'position') return 'static';
         if (k === 'width') return el ? el.offsetWidth + 'px' : '0px';
         if (k === 'height') return el ? el.offsetHeight + 'px' : '0px';
         return v;
      },
   });
};
function mediaMatches(q) {
   const [w, h] = L.viewport();
   return String(q).split(',').some(part => {
      let ok = true;
      part.replace(/\(\s*([a-z-]+)\s*(?::\s*([^)]+))?\)/g, (m, f, v) => {
         const n = parseFloat(v);
         switch (f) {
         case 'min-width': ok = ok && w >= n; break;
         case 'max-width': ok = ok && w <= n; break;
         case 'min-height': ok = ok && h >= n; break;
         case 'max-height': ok = ok && h <= n; break;
         case 'orientation': ok = ok && (v.trim() === 'landscape') === (w >= h); break;
         case 'prefers-color-scheme': ok = ok && v.trim() === 'light'; break;
         case 'prefers-reduced-motion': ok = ok && v.trim() === 'no-preference'; break;
         case 'hover': ok = ok && v.trim() === 'none'; break;
         case 'pointer': ok = ok && v.trim() === 'coarse'; break;
         case 'min-resolution': case '-webkit-min-device-pixel-ratio': ok = ok && n <= 1; break;
         default: break;
         }
      });
      if (/\bprint\b/.test(part) && !/not\s+print/.test(part)) ok = false;
      return ok;
   });
}
g.matchMedia = q => {
   const mql = new EventTarget();
   Object.defineProperty(mql, 'matches', { get: () => mediaMatches(q) });
   mql.media = String(q);
   mql.onchange = null;
   mql.addListener = f => mql.addEventListener('change', f);
   mql.removeListener = f => mql.removeEventListener('change', f);
   return mql;
};

/* --------------------------------------------------------------- location */

function parseURL(s) {
   const m = /^([a-z][a-z0-9+.-]*:)(?:\/\/(?:[^@/?#]*@)?(\[[^\]]*\]|[^:/?#]*)(?::(\d+))?)?([^?#]*)(\?[^#]*)?(#.*)?$/i.exec(s);
   if (!m) return null;
   return { protocol: m[1].toLowerCase(), hostname: m[2] || '', port: m[3] || '', pathname: m[4] || (m[2] !== undefined ? '/' : ''), search: m[5] && m[5] !== '?' ? m[5] : '', hash: m[6] && m[6] !== '#' ? m[6] : '' };
}
class URLSearchParams {
   constructor(init) {
      hide(this, '_l', []);
      if (typeof init === 'string') {
         for (const p of init.replace(/^\?/, '').split('&')) {
            if (!p) continue;
            const i = p.indexOf('=');
            const dec = s => { try { return decodeURIComponent(s.replace(/\+/g, ' ')); } catch (e) { return s; } };
            this._l.push(i < 0 ? [dec(p), ''] : [dec(p.slice(0, i)), dec(p.slice(i + 1))]);
         }
      } else if (init && typeof init[Symbol.iterator] === 'function') {
         for (const [k, v] of init) this._l.push([String(k), String(v)]);
      } else if (init && typeof init === 'object') {
         for (const k of Object.keys(init)) this._l.push([k, String(init[k])]);
      }
   }
   append(k, v) { this._l.push([String(k), String(v)]); this._u && this._u(); }
   delete(k) { this._l = this._l.filter(e => e[0] !== k); this._u && this._u(); }
   get(k) { const e = this._l.find(e => e[0] === k); return e ? e[1] : null; }
   getAll(k) { return this._l.filter(e => e[0] === k).map(e => e[1]); }
   has(k) { return this._l.some(e => e[0] === k); }
   set(k, v) { const i = this._l.findIndex(e => e[0] === k); if (i < 0) this._l.push([String(k), String(v)]); else { this._l[i][1] = String(v); this._l = this._l.filter((e, j) => j <= i || e[0] !== k); } this._u && this._u(); }
   sort() { this._l.sort((a, b) => a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0); this._u && this._u(); }
   forEach(f, t) { for (const [k, v] of this._l) f.call(t, v, k, this); }
   keys() { return this._l.map(e => e[0])[Symbol.iterator](); }
   values() { return this._l.map(e => e[1])[Symbol.iterator](); }
   entries() { return this._l.map(e => e.slice())[Symbol.iterator](); }
   [Symbol.iterator]() { return this.entries(); }
   get size() { return this._l.length; }
   toString() {
      const enc = s => encodeURIComponent(s).replace(/%20/g, '+').replace(/[!'()~]/g, c => '%' + c.charCodeAt(0).toString(16).toUpperCase());
      return this._l.map(([k, v]) => enc(k) + '=' + enc(v)).join('&');
   }
}
class URL {
   constructor(u, base) {
      const abs = L.resolve(String(u), base === undefined ? undefined : String(base));
      const p = abs && parseURL(abs);
      if (!p || (base === undefined && !/^[a-z][a-z0-9+.-]*:/i.test(String(u)))) throw new TypeError("Failed to construct 'URL': Invalid URL");
      hide(this, '_p', p);
   }
   get protocol() { return this._p.protocol; } set protocol(v) { this._p.protocol = String(v).replace(/:?$/, ':'); }
   get hostname() { return this._p.hostname; } set hostname(v) { this._p.hostname = String(v); }
   get port() { return this._p.port; } set port(v) { this._p.port = String(v); }
   get host() { return this._p.hostname + (this._p.port ? ':' + this._p.port : ''); }
   set host(v) { const [h, p] = String(v).split(':'); this._p.hostname = h; this._p.port = p || ''; }
   get pathname() { return this._p.pathname; } set pathname(v) { this._p.pathname = String(v).startsWith('/') ? String(v) : '/' + v; }
   get search() { return this._p.search; } set search(v) { v = String(v); this._p.search = v && v !== '?' ? (v[0] === '?' ? v : '?' + v) : ''; }
   get hash() { return this._p.hash; } set hash(v) { v = String(v); this._p.hash = v && v !== '#' ? (v[0] === '#' ? v : '#' + v) : ''; }
   get origin() { return /^(https?|wss?|ftp):$/.test(this._p.protocol) ? this._p.protocol + '//' + this.host : 'null'; }
   get username() { return ''; } get password() { return ''; }
   get searchParams() {
      const sp = new URLSearchParams(this._p.search);
      hide(sp, '_u', () => { const s = sp.toString(); this._p.search = s ? '?' + s : ''; });
      return sp;
   }
   get href() { const p = this._p; return p.protocol + (p.hostname || p.protocol === 'file:' ? '//' + this.host : '') + p.pathname + p.search + p.hash; }
   set href(v) { const n = new URL(v); this._p = n._p; }
   toString() { return this.href; }
   toJSON() { return this.href; }
   static createObjectURL(b) { return 'data:' + (b.type || 'application/octet-stream') + ';base64,' + btoa(b._bin || ''); }
   static revokeObjectURL() {}
   static canParse(u, b) { try { new URL(u, b); return true; } catch (e) { return false; } }
}
const location = {
   get href() { return L.url(); }, set href(v) { L.navigate(String(v)); },
   get _u() { return new URL(L.url()); },
   get protocol() { return this._u.protocol; }, get host() { return this._u.host; },
   get hostname() { return this._u.hostname; }, get port() { return this._u.port; },
   get pathname() { return this._u.pathname; }, get search() { return this._u.search; },
   set search(v) { const u = this._u; u.search = v; L.navigate(u.href); },
   get hash() { return this._u.hash; },
   set hash(v) { const old = L.url(); const u = this._u; u.hash = v; hashTo(u.href, old); },
   get origin() { return this._u.origin; },
   get ancestorOrigins() { return []; },
   assign(u) { L.navigate(String(u)); }, replace(u) { L.navigate(String(u)); },
   reload() { L.navigate(L.url()); },
   toString() { return L.url(); },
};
function setURL(u) {
   const abs = L.resolve(String(u));
   if (!abs || new URL(abs).origin !== location.origin) throw new DOMException('cross-origin history URL', 'SecurityError');
   L.setURL(abs);
}
function hashTo(href) { history.pushState(null, '', href); dispatch(g, new Event('hashchange')); }
g.location = location;
const history = {
   _states: [null], _i: 0,
   get length() { return this._states.length; },
   get state() { return this._states[this._i]; },
   scrollRestoration: 'auto',
   /* Same-document URL changes: the address changes, nothing loads. */
   pushState(s, t, u) { this._states.length = this._i + 1; this._states.push(s); this._i++; if (u !== undefined && u !== null) setURL(u); },
   replaceState(s, t, u) { this._states[this._i] = s; if (u !== undefined && u !== null) setURL(u); },
   back() { this.go(-1); }, forward() { this.go(1); },
   go(n = 0) {
      const i = this._i + n;
      if (i < 0 || i >= this._states.length || n === 0) return;
      this._i = i;
      setTimeout(() => dispatch(g, new PopStateEvent('popstate', { state: this.state })), 0);
   },
};
g.history = history;

g.navigator = {
   get userAgent() { return L.userAgent; },
   get appVersion() { return L.userAgent.replace(/^Mozilla\//, ''); },
   appName: 'Netscape', appCodeName: 'Mozilla', product: 'Gecko', productSub: '20030107',
   platform: 'Linux armv8l', vendor: 'Google Inc.', vendorSub: '',
   language: 'ja-JP', languages: ['ja-JP', 'ja', 'en-US', 'en'],
   onLine: true, cookieEnabled: true, doNotTrack: null, webdriver: false, pdfViewerEnabled: false,
   maxTouchPoints: 5, hardwareConcurrency: 8, deviceMemory: 4,
   plugins: [], mimeTypes: [],
   connection: { effectiveType: '4g', downlink: 10, rtt: 50, saveData: false, type: 'wifi', addEventListener() {} },
   clipboard: { writeText: () => Promise.resolve(), readText: () => Promise.resolve('') },
   permissions: { query: () => Promise.resolve({ state: 'prompt', addEventListener() {} }) },
   sendBeacon(u, d) { setTimeout(() => L.http('POST', String(u), '', d === undefined ? null : String(d), false), 0); return true; },
   vibrate() { return false; },
   share: () => Promise.reject(new DOMException('share is not available', 'NotAllowedError')),
   javaEnabled: () => false,
   getGamepads: () => [],
   userActivation: { hasBeenActive: true, isActive: true },
};

/* ---------------------------------------------------------------- storage */

class Storage {
   constructor(persist) {
      hide(this, '_persist', persist);
      let data = {};
      if (persist) { try { data = JSON.parse(L.storage() || '{}') || {}; } catch (e) { data = {}; } }
      hide(this, '_d', data);
      return new Proxy(this, {
         get(t, k) { if (k in t || typeof k !== 'string') { const v = t[k]; return typeof v === 'function' ? v.bind(t) : v; } return Object.prototype.hasOwnProperty.call(t._d, k) ? t._d[k] : undefined; },
         set(t, k, v) { t.setItem(k, v); return true; },
         deleteProperty(t, k) { t.removeItem(k); return true; },
         ownKeys(t) { return Object.keys(t._d); },
         getOwnPropertyDescriptor(t, k) { return Object.prototype.hasOwnProperty.call(t._d, k) ? { value: t._d[k], enumerable: true, configurable: true } : undefined; },
      });
   }
   _save() { if (this._persist) L.storage(JSON.stringify(this._d)); }
   get length() { return Object.keys(this._d).length; }
   key(i) { return Object.keys(this._d)[i] || null; }
   getItem(k) { k = String(k); return Object.prototype.hasOwnProperty.call(this._d, k) ? this._d[k] : null; }
   setItem(k, v) { this._d[String(k)] = String(v); this._save(); }
   removeItem(k) { delete this._d[String(k)]; this._save(); }
   clear() { for (const k of Object.keys(this._d)) delete this._d[k]; this._save(); }
}
let local = null, session = null;
Object.defineProperty(g, 'localStorage', { get: () => local || (local = new Storage(true)), configurable: true });
Object.defineProperty(g, 'sessionStorage', { get: () => session || (session = new Storage(false)), configurable: true });
g.indexedDB = undefined;

/* ----------------------------------------------------------------- timers */

const timers = new Map();
let timerSeq = 0;
function addTimer(fn, ms, args, repeat) {
   const id = ++timerSeq;
   ms = Math.max(0, +ms || 0);
   timers.set(id, { fn, args, due: L.now() + ms, ms: repeat ? Math.max(ms, 4) : 0, seq: id });
   return id;
}
g.setTimeout = (fn, ms, ...args) => addTimer(fn, ms, args, false);
g.setInterval = (fn, ms, ...args) => addTimer(fn, ms, args, true);
g.clearTimeout = g.clearInterval = id => { timers.delete(id); };
const rafs = new Map();
let rafSeq = 0, lastFrame = 0;
g.requestAnimationFrame = fn => { rafs.set(++rafSeq, fn); return rafSeq; };
g.cancelAnimationFrame = id => { rafs.delete(id); };
g.requestIdleCallback = (fn) => setTimeout(() => fn({ didTimeout: false, timeRemaining: () => 10 }), 1);
g.cancelIdleCallback = id => clearTimeout(id);
g.queueMicrotask = fn => { NativePromise.resolve().then(fn).catch(reportError); };
g.structuredClone = v => v === undefined ? undefined : JSON.parse(JSON.stringify(v));

g.__lb_tick = function (now) {
   /* Only what was due when this tick began: a timer that re-arms itself at
    * 0ms runs on the next tick, not in a loop here. */
   const due = [...timers.entries()].filter(([, t]) => t.due <= now).sort((a, b) => a[1].due - b[1].due || a[1].seq - b[1].seq);
   for (const [id, t] of due) {
      if (!timers.has(id)) continue;
      if (t.ms) t.due = now + t.ms; else timers.delete(id);
      try {
         if (typeof t.fn === 'function') t.fn.apply(g, t.args);
         else (0, eval)(String(t.fn));
      } catch (e) { reportError(e); }
   }
   if (rafs.size && now - lastFrame >= 16) {
      lastFrame = now;
      const cbs = [...rafs.values()];
      rafs.clear();
      const ts = now - T0;
      for (const f of cbs) { try { f(ts); } catch (e) { reportError(e); } }
      L.redraw();
   }
   let next = rafs.size ? Math.max(0, 16 - (L.now() - lastFrame)) : -1;
   for (const t of timers.values()) { const d = Math.max(0, t.due - L.now()); if (next < 0 || d < next) next = d; }
   return next;
};

g.performance = {
   now: () => L.now() - T0,
   timeOrigin: Date.now() - (L.now() - T0),
   timing: { navigationStart: Date.now(), fetchStart: Date.now(), domLoading: Date.now(), domComplete: 0, loadEventEnd: 0 },
   navigation: { type: 0, redirectCount: 0 },
   mark() {}, measure() {}, clearMarks() {}, clearMeasures() {},
   getEntries: () => [], getEntriesByType: () => [], getEntriesByName: () => [],
   memory: { usedJSHeapSize: 0, totalJSHeapSize: 0, jsHeapSizeLimit: 0 },
   toJSON() { return {}; },
};

/* ------------------------------------------------------- text and binary */

g.btoa = s => {
   s = String(s);
   const t = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
   let o = '';
   for (let i = 0; i < s.length; i += 3) {
      const a = s.charCodeAt(i), b = s.charCodeAt(i + 1), c = s.charCodeAt(i + 2);
      if (a > 255 || b > 255 || c > 255) throw new DOMException('Invalid character', 'InvalidCharacterError');
      const v = (a << 16) | ((b || 0) << 8) | (c || 0);
      o += t[v >> 18 & 63] + t[v >> 12 & 63] + (i + 1 < s.length ? t[v >> 6 & 63] : '=') + (i + 2 < s.length ? t[v & 63] : '=');
   }
   return o;
};
g.atob = s => {
   s = String(s).replace(/[\s=]/g, '');
   const t = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
   let o = '', v = 0, bits = 0;
   for (const ch of s) {
      const i = t.indexOf(ch === '-' ? '+' : ch === '_' ? '/' : ch);
      if (i < 0) throw new DOMException('Invalid character', 'InvalidCharacterError');
      v = (v << 6) | i; bits += 6;
      if (bits >= 8) { bits -= 8; o += String.fromCharCode((v >> bits) & 255); }
   }
   return o;
};
class TextEncoder {
   get encoding() { return 'utf-8'; }
   encode(s = '') {
      s = String(s);
      const out = [];
      for (let i = 0; i < s.length; i++) {
         let c = s.codePointAt(i);
         if (c > 0xffff) i++;
         if (c < 0x80) out.push(c);
         else if (c < 0x800) out.push(0xc0 | c >> 6, 0x80 | c & 63);
         else if (c < 0x10000) out.push(0xe0 | c >> 12, 0x80 | c >> 6 & 63, 0x80 | c & 63);
         else out.push(0xf0 | c >> 18, 0x80 | c >> 12 & 63, 0x80 | c >> 6 & 63, 0x80 | c & 63);
      }
      return new Uint8Array(out);
   }
   encodeInto(s, dst) { const b = this.encode(s); dst.set(b.subarray(0, dst.length)); return { read: s.length, written: Math.min(b.length, dst.length) }; }
}
class TextDecoder {
   constructor(label = 'utf-8') { this.encoding = String(label).toLowerCase(); }
   decode(buf) {
      if (!buf) return '';
      const b = buf instanceof ArrayBuffer ? new Uint8Array(buf) : new Uint8Array(buf.buffer, buf.byteOffset, buf.byteLength);
      if (this.encoding !== 'utf-8' && this.encoding !== 'utf8') return String.fromCharCode(...b);
      let s = '';
      for (let i = 0; i < b.length;) {
         let c = b[i++];
         if (c >= 0xf0) c = (c & 7) << 18 | (b[i++] & 63) << 12 | (b[i++] & 63) << 6 | (b[i++] & 63);
         else if (c >= 0xe0) c = (c & 15) << 12 | (b[i++] & 63) << 6 | (b[i++] & 63);
         else if (c >= 0xc0) c = (c & 31) << 6 | (b[i++] & 63);
         s += String.fromCodePoint(c);
      }
      return s;
   }
}
const utf8 = s => { let o = ''; for (const b of new TextEncoder().encode(s)) o += String.fromCharCode(b); return o; };
const fromUtf8 = s => new TextDecoder().decode(new Uint8Array([...s].map(c => c.charCodeAt(0))));

class Blob {
   constructor(parts = [], opt = {}) {
      let bin = '';
      for (const p of parts) {
         if (p instanceof Blob) bin += p._bin;
         else if (p instanceof ArrayBuffer) bin += String.fromCharCode(...new Uint8Array(p));
         else if (ArrayBuffer.isView(p)) bin += String.fromCharCode(...new Uint8Array(p.buffer, p.byteOffset, p.byteLength));
         else bin += utf8(String(p));
      }
      hide(this, '_bin', bin);
      this.type = opt.type || '';
   }
   get size() { return this._bin.length; }
   text() { return Promise.resolve(fromUtf8(this._bin)); }
   arrayBuffer() { return Promise.resolve(new Uint8Array([...this._bin].map(c => c.charCodeAt(0))).buffer); }
   slice(a, b, t) { const n = new Blob([], { type: t || this.type }); n._bin = this._bin.slice(a, b); return n; }
}
class File extends Blob { constructor(parts, name, opt = {}) { super(parts, opt); this.name = name; this.lastModified = Date.now(); } }
class FileReader extends EventTarget {
   readAsText(b) { this._done(fromUtf8(b._bin)); }
   readAsDataURL(b) { this._done('data:' + (b.type || 'application/octet-stream') + ';base64,' + btoa(b._bin)); }
   readAsArrayBuffer(b) { this._done(new Uint8Array([...b._bin].map(c => c.charCodeAt(0))).buffer); }
   _done(r) { setTimeout(() => { this.result = r; this.readyState = 2; const e = new ProgressEvent('load'); this.dispatchEvent(e); this.dispatchEvent(new ProgressEvent('loadend')); }, 0); }
   abort() {}
}
class FormData {
   constructor(form) {
      hide(this, '_l', []);
      if (form && form.elements) for (const el of form.elements) {
         const n = el.getAttribute('name');
         if (!n || el.hasAttribute('disabled')) continue;
         const t = (el.getAttribute('type') || '').toLowerCase();
         if ((t === 'checkbox' || t === 'radio') && !el.checked) continue;
         if (t === 'submit' || t === 'button') continue;
         this._l.push([n, el.value]);
      }
   }
   append(k, v) { this._l.push([String(k), v]); }
   set(k, v) { this.delete(k); this.append(k, v); }
   get(k) { const e = this._l.find(e => e[0] === k); return e ? e[1] : null; }
   getAll(k) { return this._l.filter(e => e[0] === k).map(e => e[1]); }
   has(k) { return this._l.some(e => e[0] === k); }
   delete(k) { this._l = this._l.filter(e => e[0] !== k); }
   entries() { return this._l[Symbol.iterator](); }
   [Symbol.iterator]() { return this.entries(); }
   forEach(f, t) { for (const [k, v] of this._l) f.call(t, v, k, this); }
}

g.crypto = {
   getRandomValues(a) {
      const b = new Uint8Array(L.random(a.byteLength));
      new Uint8Array(a.buffer, a.byteOffset, a.byteLength).set(b);
      return a;
   },
   randomUUID() {
      const b = new Uint8Array(L.random(16));
      b[6] = (b[6] & 15) | 64; b[8] = (b[8] & 63) | 128;
      const h = [...b].map(x => x.toString(16).padStart(2, '0')).join('');
      return h.slice(0, 8) + '-' + h.slice(8, 12) + '-' + h.slice(12, 16) + '-' + h.slice(16, 20) + '-' + h.slice(20);
   },
};

/* As natively: name and code are prototype getters over internal state, so
 * a polyfill that re-derives DOMException (core-js) sees the shape it
 * expects instead of hitting its own getters with an assignment. */
const DOM_CODES = { IndexSizeError: 1, HierarchyRequestError: 3, WrongDocumentError: 4,
   InvalidCharacterError: 5, NoModificationAllowedError: 7, NotFoundError: 8, NotSupportedError: 9,
   InvalidStateError: 11, SyntaxError: 12, InvalidModificationError: 13, NamespaceError: 14,
   InvalidAccessError: 15, SecurityError: 18, NetworkError: 19, AbortError: 20, URLMismatchError: 21,
   QuotaExceededError: 22, TimeoutError: 23, InvalidNodeTypeError: 24, DataCloneError: 25 };
class DOMException extends Error {
   constructor(message = '', name = 'Error') {
      super(String(message));
      hide(this, '_name', String(name));
   }
   get name() { return this._name; }
   get code() { return DOM_CODES[this._name] || 0; }
}
for (const [k, v] of Object.entries(DOM_CODES))
   Object.defineProperty(DOMException, k.replace(/([a-z])([A-Z])/g, '$1_$2').toUpperCase().replace(/_ERROR$/, '_ERR'),
                         { value: v, enumerable: true });

/* ---------------------------------------------------------------- network */

class Headers {
   constructor(init) {
      hide(this, '_m', new Map());
      if (init instanceof Headers) init.forEach((v, k) => this.append(k, v));
      else if (Array.isArray(init)) for (const [k, v] of init) this.append(k, v);
      else if (init && typeof init === 'object') for (const k of Object.keys(init)) this.append(k, init[k]);
   }
   append(k, v) { k = String(k).toLowerCase(); const o = this._m.get(k); this._m.set(k, o === undefined ? String(v) : o + ', ' + v); }
   set(k, v) { this._m.set(String(k).toLowerCase(), String(v)); }
   get(k) { const v = this._m.get(String(k).toLowerCase()); return v === undefined ? null : v; }
   has(k) { return this._m.has(String(k).toLowerCase()); }
   delete(k) { this._m.delete(String(k).toLowerCase()); }
   forEach(f, t) { for (const [k, v] of this._m) f.call(t, v, k, this); }
   entries() { return this._m.entries(); } keys() { return this._m.keys(); } values() { return this._m.values(); }
   [Symbol.iterator]() { return this._m.entries(); }
}
function parseHeaders(text) {
   const h = new Headers();
   for (const line of String(text || '').split('\n')) {
      const i = line.indexOf(':');
      if (i > 0) h.append(line.slice(0, i).trim(), line.slice(i + 1).trim());
   }
   return h;
}
function bodyString(body) {
   if (body === undefined || body === null) return null;
   if (typeof body === 'string') return body;
   if (body instanceof URLSearchParams) return body.toString();
   if (body instanceof FormData) return new URLSearchParams(body._l.map(([k, v]) => [k, typeof v === 'string' ? v : ''])).toString();
   if (body instanceof Blob) return fromUtf8(body._bin);
   if (body instanceof ArrayBuffer || ArrayBuffer.isView(body)) return new TextDecoder().decode(body);
   return String(body);
}
function headerText(h, body) {
   let t = '';
   h.forEach((v, k) => { t += k + ': ' + v + '\r\n'; });
   if (body instanceof URLSearchParams && !h.has('content-type')) t += 'content-type: application/x-www-form-urlencoded;charset=UTF-8\r\n';
   else if (body instanceof FormData && !h.has('content-type')) t += 'content-type: application/x-www-form-urlencoded;charset=UTF-8\r\n';
   else if (typeof body === 'string' && !h.has('content-type')) t += 'content-type: text/plain;charset=UTF-8\r\n';
   return t;
}

class Response {
   constructor(body = null, init = {}) {
      hide(this, '_body', body === null || body === undefined ? '' : body);
      this.status = init.status === undefined ? 200 : init.status;
      this.statusText = init.statusText || '';
      this.headers = init.headers instanceof Headers ? init.headers : new Headers(init.headers);
      this.url = init.url || '';
      this.redirected = false;
      this.type = 'basic';
      this.bodyUsed = false;
   }
   get ok() { return this.status >= 200 && this.status < 300; }
   _take() { if (this.bodyUsed) return Promise.reject(new TypeError('body already used')); this.bodyUsed = true; return Promise.resolve(this._body); }
   text() { return this._take().then(b => typeof b === 'string' ? b : new TextDecoder().decode(b)); }
   json() { return this.text().then(JSON.parse); }
   arrayBuffer() { return this._take().then(b => typeof b === 'string' ? new TextEncoder().encode(b).buffer : b); }
   blob() { return this.arrayBuffer().then(b => new Blob([b], { type: this.headers.get('content-type') || '' })); }
   clone() { return new Response(this._body, this); }
   static json(v, init) { return new Response(JSON.stringify(v), Object.assign({ headers: { 'content-type': 'application/json' } }, init)); }
   static error() { return new Response(null, { status: 0 }); }
}
class Request {
   constructor(input, init = {}) {
      this.url = L.resolve(String(input instanceof Request ? input.url : input)) || String(input);
      this.method = String(init.method || (input instanceof Request ? input.method : 'GET')).toUpperCase();
      this.headers = new Headers(init.headers || (input instanceof Request ? input.headers : undefined));
      hide(this, '_body', init.body !== undefined ? init.body : input instanceof Request ? input._body : null);
      this.credentials = init.credentials || 'same-origin';
      this.mode = init.mode || 'cors';
      this.signal = init.signal || null;
   }
   clone() { return new Request(this); }
}
class AbortSignal extends EventTarget {
   constructor() { super(); this.aborted = false; this.reason = undefined; this.onabort = null; }
   throwIfAborted() { if (this.aborted) throw this.reason; }
   static timeout(ms) { const c = new AbortController(); setTimeout(() => c.abort(new DOMException('timeout', 'TimeoutError')), ms); return c.signal; }
   static abort(r) { const c = new AbortController(); c.abort(r); return c.signal; }
}
class AbortController {
   constructor() { this.signal = new AbortSignal(); }
   abort(reason) {
      if (this.signal.aborted) return;
      this.signal.aborted = true;
      this.signal.reason = reason === undefined ? new DOMException('The operation was aborted.', 'AbortError') : reason;
      dispatch(this.signal, new Event('abort'));
   }
}
/* Requests in flight: id -> completion. */
const httpWaiting = new Map();
g.__lb_http_done = function (id, r) {
   const done = httpWaiting.get(id);
   httpWaiting.delete(id);
   if (done) { try { done(r); } catch (e) { reportError(e); } }
};
function httpAsync(method, url, headers, body, binary, done) {
   httpWaiting.set(L.httpAsync(method, url, headers, body, binary), done);
}

g.fetch = (input, init = {}) => new NativePromise((resolve, reject) => {
   const req = new Request(input, init);
   if (req.signal && req.signal.aborted) { reject(req.signal.reason); return; }
   const body = bodyString(req._body);
   httpAsync(req.method, req.url, headerText(req.headers, req._body), body, true, r => {
      if (req.signal && req.signal.aborted) { reject(req.signal.reason); return; }
      if (r.error && !r.status) { reject(new TypeError('Failed to fetch')); return; }
      resolve(new Response(r.body, { status: r.status, headers: parseHeaders(r.headers), url: r.url }));
   });
});

class XMLHttpRequest extends EventTarget {
   constructor() {
      super();
      this.readyState = 0; this.status = 0; this.statusText = ''; this.response = null; this.responseText = '';
      this.responseType = ''; this.responseURL = ''; this.timeout = 0; this.withCredentials = false;
      this.upload = new EventTarget();
      hide(this, '_h', new Headers());
      hide(this, '_rh', new Headers());
   }
   open(method, url, async = true) {
      hide(this, '_m', String(method).toUpperCase());
      hide(this, '_u', L.resolve(String(url)) || String(url));
      hide(this, '_async', async !== false);
      this._state(1);
   }
   setRequestHeader(k, v) { this._h.append(k, v); }
   getResponseHeader(k) { return this._rh.get(k); }
   getAllResponseHeaders() { let t = ''; this._rh.forEach((v, k) => { t += k + ': ' + v + '\r\n'; }); return t; }
   overrideMimeType() {}
   abort() { hide(this, '_aborted', true); }
   _state(s) { this.readyState = s; this.dispatchEvent(new Event('readystatechange')); }
   send(body) {
      const bin = this.responseType === 'arraybuffer' || this.responseType === 'blob';
      const finish = r => {
         if (this._aborted) { this.dispatchEvent(new ProgressEvent('abort')); return; }
         if (r.error && !r.status) {
            this.status = 0; this._state(4);
            this.dispatchEvent(new ProgressEvent('error'));
            this.dispatchEvent(new ProgressEvent('loadend'));
            return;
         }
         this.status = r.status; this.responseURL = r.url;
         this.statusText = r.status === 200 ? 'OK' : '';
         hide(this, '_rh', parseHeaders(r.headers));
         this._state(2); this._state(3);
         if (this.responseType === 'arraybuffer') this.response = r.body;
         else if (this.responseType === 'blob') this.response = new Blob([r.body], { type: this._rh.get('content-type') || '' });
         else {
            this.responseText = r.body;
            if (this.responseType === 'json') { try { this.response = JSON.parse(r.body); } catch (e) { this.response = null; } }
            else this.response = r.body;
         }
         this._state(4);
         const n = typeof r.body === 'string' ? r.body.length : r.body.byteLength;
         this.dispatchEvent(new ProgressEvent('load', { loaded: n, total: n, lengthComputable: true }));
         this.dispatchEvent(new ProgressEvent('loadend', { loaded: n, total: n }));
      };
      if (this._async) {
         setTimeout(() => this.dispatchEvent(new ProgressEvent('loadstart')), 0);
         httpAsync(this._m, this._u, headerText(this._h, body), bodyString(body), bin, finish);
      } else {
         this.dispatchEvent(new ProgressEvent('loadstart'));
         finish(L.http(this._m, this._u, headerText(this._h, body), bodyString(body), bin));
      }
   }
}
XMLHttpRequest.UNSENT = 0; XMLHttpRequest.OPENED = 1; XMLHttpRequest.HEADERS_RECEIVED = 2; XMLHttpRequest.LOADING = 3; XMLHttpRequest.DONE = 4;

class WebSocket extends EventTarget {
   constructor(url) {
      super();
      this.url = url; this.readyState = 3; this.protocol = ''; this.extensions = ''; this.bufferedAmount = 0; this.binaryType = 'blob';
      setTimeout(() => { this.dispatchEvent(new Event('error')); this.dispatchEvent(Object.assign(new Event('close'), { code: 1006, reason: '', wasClean: false })); }, 0);
   }
   send() { throw new DOMException('WebSocket is not open', 'InvalidStateError'); }
   close() {}
}
WebSocket.CONNECTING = 0; WebSocket.OPEN = 1; WebSocket.CLOSING = 2; WebSocket.CLOSED = 3;

/* ------------------------------------------------------------- observers */

class MutationObserver { constructor(cb) { this._cb = cb; } observe() {} disconnect() {} takeRecords() { return []; } }
class ResizeObserver {
   constructor(cb) { this._cb = cb; }
   observe(el) { setTimeout(() => { const r = el.getBoundingClientRect(); try { this._cb([{ target: el, contentRect: r, borderBoxSize: [{ inlineSize: r.width, blockSize: r.height }], contentBoxSize: [{ inlineSize: r.width, blockSize: r.height }] }], this); } catch (e) { reportError(e); } }, 0); }
   unobserve() {} disconnect() {}
}
class IntersectionObserver {
   constructor(cb, opt = {}) { this._cb = cb; this.root = opt.root || null; this.rootMargin = opt.rootMargin || '0px'; this.thresholds = [].concat(opt.threshold || 0); }
   observe(el) { setTimeout(() => { const r = el.getBoundingClientRect(); try { this._cb([{ target: el, isIntersecting: true, intersectionRatio: 1, boundingClientRect: r, intersectionRect: r, rootBounds: null, time: performance.now() }], this); } catch (e) { reportError(e); } }, 0); }
   unobserve() {} disconnect() {} takeRecords() { return []; }
}
class Image { constructor(w, h) { const i = document.createElement('img'); if (w) i.width = w; if (h) i.height = h; return i; } }
class Audio { constructor(src) { const a = document.createElement('audio'); if (src) a.setAttribute('src', src); return a; } }
const CSS = {
   supports: (p, v) => v === undefined ? !/^\s*selector\(/.test(p) : !/^(display:\s*contents|-webkit-)/.test(p),
   escape: s => String(s).replace(/([^a-zA-Z0-9_\u00a0-\uffff-])/g, '\\$1').replace(/^(\d)/, '\\3$1 '),
};
g.CSS = CSS;

/* Constructors pages test for or instantiate. */
Object.assign(g, {
   Event, CustomEvent, UIEvent, MouseEvent, PointerEvent, WheelEvent, KeyboardEvent, FocusEvent, InputEvent,
   TouchEvent, Touch, MessageEvent, ProgressEvent, ErrorEvent, PopStateEvent, EventTarget,
   Node, Element, HTMLElement, Text, Comment, CharacterData, Document, HTMLDocument: Document, DocumentFragment,
   NodeList, HTMLCollection: NodeList, DOMTokenList, DOMRect, DOMException,
   HTMLInputElement, HTMLTextAreaElement, HTMLSelectElement, HTMLOptionElement, HTMLButtonElement,
   HTMLAnchorElement, HTMLImageElement, HTMLFormElement, HTMLScriptElement, HTMLStyleElement, HTMLLinkElement,
   HTMLMetaElement, HTMLCanvasElement, HTMLMediaElement, HTMLVideoElement, HTMLAudioElement, HTMLIFrameElement,
   HTMLTemplateElement, HTMLLabelElement, HTMLBodyElement, HTMLHeadElement, HTMLHtmlElement, HTMLDivElement,
   HTMLSpanElement, HTMLParagraphElement, HTMLUListElement, HTMLLIElement, HTMLTitleElement, SVGElement,
   URL, URLSearchParams, TextEncoder, TextDecoder, Blob, File, FileReader, FormData, Headers, Request, Response,
   AbortController, AbortSignal, XMLHttpRequest, WebSocket, Storage,
   MutationObserver, ResizeObserver, IntersectionObserver, Image, Audio, Window: Object,
});

/* -------------------------------------------------------------- input */

let pressTarget = null, focusValue = null;
const target = uid => W(uid) || document.body || document;

function pointerInit(x, y, touch, button) {
   return { bubbles: true, cancelable: true, composed: true, clientX: x, clientY: y, screenX: x, screenY: y,
            button: button || 0, buttons: 1, pointerType: touch ? 'touch' : 'mouse', pressure: 0.5 };
}
function touchInit(t, x, y, end) {
   const touch = new Touch({ identifier: 0, target: t, clientX: x, clientY: y, pageX: x, pageY: y, screenX: x, screenY: y });
   return { bubbles: true, cancelable: true, composed: true, touches: end ? [] : [touch], targetTouches: end ? [] : [touch], changedTouches: [touch] };
}
function commonAncestor(a, b) {
   if (!a || !b) return b || a;
   for (let n = a; n; n = n.parentNode) if (n.contains && n.contains(b)) return n;
   return b;
}
/* The default action of a click. */
function activate(t) {
   const a = t.closest ? t.closest('a[href]') : null;
   if (a) {
      const href = a.getAttribute('href');
      if (href && !/^javascript:/i.test(href)) {
         if (href[0] === '#') hashTo(L.resolve(href));
         else location.assign(href);
      } else if (href) { try { (0, eval)(decodeURIComponent(href.slice(11))); } catch (e) { reportError(e); } }
      return;
   }
   const btn = t.closest ? t.closest('button,input[type=submit],input[type=image]') : null;
   if (btn && ((btn.getAttribute('type') || 'submit').toLowerCase() === 'submit')) {
      const f = btn.closest('form');
      if (f) f.requestSubmit();
   }
   const box = t.localName === 'input' && /^(checkbox|radio)$/i.test(t.getAttribute('type') || '') ? t : null;
   if (box) {
      if (box.getAttribute('type').toLowerCase() === 'radio') {
         const n = box.getAttribute('name');
         if (n) for (const o of document.querySelectorAll('input[type=radio][name="' + n + '"]')) o.checked = false;
         box.checked = true;
      } else box.checked = !box.checked;
      dispatch(box, new Event('input', { bubbles: true }));
      dispatch(box, new Event('change', { bubbles: true }));
   }
   const label = t.closest ? t.closest('label') : null;
   if (label && !box) { const c = label.control; if (c && c !== t) c.click(); }
}
function submitForm(f) {
   const fd = new FormData(f);
   const q = new URLSearchParams(fd._l.map(([k, v]) => [k, String(v)])).toString();
   const action = f.getAttribute('action') ? L.resolve(f.getAttribute('action')) : L.url().replace(/[?#].*$/, '');
   const method = (f.getAttribute('method') || 'get').toLowerCase();
   if (method === 'post') {
      const r = L.http('POST', action, 'content-type: application/x-www-form-urlencoded\r\n', q, false);
      L.navigate(r.url || action);
   } else L.navigate(action.replace(/\?.*$/, '') + '?' + q);
}

g.__lb_pointer = function (kind, uid, x, y, button, dragged, touch) {
   const t = target(uid);
   if (kind === 1) {
      pressTarget = t;
      dispatch(t, new PointerEvent('pointerdown', pointerInit(x, y, touch, button)));
      if (touch) dispatch(t, new TouchEvent('touchstart', touchInit(t, x, y)));
      dispatch(t, new MouseEvent('mousedown', pointerInit(x, y, touch, button)));
   } else if (kind === 3) {
      const tt = pressTarget || t;
      dispatch(t, new PointerEvent('pointermove', pointerInit(x, y, touch)));
      if (touch) dispatch(tt, new TouchEvent('touchmove', touchInit(tt, x, y)));
      dispatch(t, new MouseEvent('mousemove', pointerInit(x, y, touch)));
   } else if (kind === 2) {
      dispatch(t, new PointerEvent('pointerup', pointerInit(x, y, touch, button)));
      if (touch) dispatch(pressTarget || t, new TouchEvent('touchend', touchInit(pressTarget || t, x, y, true)));
      dispatch(t, new MouseEvent('mouseup', pointerInit(x, y, touch, button)));
      if (!dragged) {
         const c = commonAncestor(pressTarget, t) || t;
         if (dispatch(c, new MouseEvent('click', Object.assign(pointerInit(x, y, touch, button), { detail: 1 })))) activate(c);
      }
      pressTarget = null;
   }
};
g.__lb_wheel = function (uid, x, y, dx, dy) {
   dispatch(target(uid), new WheelEvent('wheel', Object.assign(pointerInit(x, y), { deltaX: dx, deltaY: dy })));
   dispatch(document, new Event('scroll'));
};
g.__lb_focus = function (from, to) {
   const a = W(from), b = W(to);
   if (a) {
      if (focusValue !== null && a.value !== undefined && a.value !== focusValue) dispatch(a, new Event('change', { bubbles: true }));
      dispatch(a, new FocusEvent('blur', { relatedTarget: b }));
      dispatch(a, new FocusEvent('focusout', { bubbles: true, relatedTarget: b }));
   }
   focusValue = b && b.value !== undefined ? b.value : null;
   if (b) {
      dispatch(b, new FocusEvent('focus', { relatedTarget: a }));
      dispatch(b, new FocusEvent('focusin', { bubbles: true, relatedTarget: a }));
   }
};
g.__lb_input = function (uid) {
   const t = W(uid);
   if (t) dispatch(t, new InputEvent('input', { bubbles: true, inputType: 'insertText' }));
};
g.__lb_key = function (type, key, code) {
   const t = W(L.focus()) || document.body || document;
   const ev = new KeyboardEvent(type, { bubbles: true, cancelable: true, key, code: key.length === 1 ? 'Key' + key.toUpperCase() : key, keyCode: code });
   const ok = dispatch(t, ev);
   if (ok && type === 'keydown' && key === 'Enter' && t.localName === 'input') {
      const f = t.closest('form');
      if (f) f.requestSubmit();
   }
   return !ok;
};
g.__lb_resize = function () { dispatch(g, new UIEvent('resize')); };
g.__lb_ready = function (stage) {
   if (stage === 1) {
      document._ready = 'interactive';
      dispatch(document, new Event('readystatechange'));
      dispatch(document, new Event('DOMContentLoaded', { bubbles: true }));
   } else {
      document._ready = 'complete';
      performance.timing.domComplete = performance.timing.loadEventEnd = Date.now();
      dispatch(document, new Event('readystatechange'));
      const b = document.body;
      const src = b && b.getAttribute('onload');
      if (src && typeof g.onload !== 'function') { try { g.onload = new Function('event', src); } catch (e) { reportError(e); } }
      dispatch(g, new Event('load'));
      dispatch(g, new Event('pageshow'));
   }
};
g.__lb_add_binding = function (name) {
   g[name] = function (payload) { L.binding(name, String(payload)); };
};

/* Runtime.evaluate's RemoteObject. */
g.__lb_remote = function (v, byValue, threw) {
   const t = typeof v;
   const obj = { type: t === 'bigint' ? 'bigint' : t };
   if (v === null) { obj.type = 'object'; obj.subtype = 'null'; obj.value = null; }
   else if (t === 'undefined') {}
   else if (t === 'number') { obj.value = v; obj.description = String(v); if (!isFinite(v) || Object.is(v, -0)) { delete obj.value; obj.unserializableValue = String(v); } }
   else if (t === 'string' || t === 'boolean') obj.value = v;
   else if (t === 'bigint') obj.unserializableValue = v + 'n';
   else if (t === 'function') { obj.className = 'Function'; obj.description = String(v).slice(0, 200); }
   else if (t === 'symbol') obj.description = String(v);
   else {
      obj.className = v && v.constructor && v.constructor.name || 'Object';
      if (Array.isArray(v)) obj.subtype = 'array';
      else if (v instanceof Error) obj.subtype = 'error';
      else if (v && v.nodeType) obj.subtype = 'node';
      obj.description = v instanceof Error ? v.name + ': ' + v.message : obj.subtype === 'node' ? v.nodeName.toLowerCase() : obj.className;
      if (byValue) { try { obj.value = JSON.parse(JSON.stringify(v)); } catch (e) { obj.value = {}; } }
   }
   const out = { result: obj };
   if (threw) out.exceptionDetails = { exceptionId: 1, text: 'Uncaught', lineNumber: 0, columnNumber: 0,
                                       exception: obj };
   return JSON.stringify(out);
};

g.addEventListener('error', () => {});
})(globalThis);
