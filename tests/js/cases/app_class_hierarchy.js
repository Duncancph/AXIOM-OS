// Class hierarchy: shapes with abstract methods, mixins, static registries, serialization
class Shape {
  static registry = new Map();
  static register(cls) { Shape.registry.set(cls.name, cls); return cls; }
  static fromJSON(json) { const { type, ...props } = JSON.parse(json); const C = Shape.registry.get(type); if (!C) throw new TypeError('unknown shape ' + type); return Object.assign(Object.create(C.prototype), props); }
  constructor(name) { if (new.target === Shape) throw new TypeError('Shape is abstract'); this.name = name; }
  area() { throw new Error('not implemented'); }
  perimeter() { return NaN; }
  toJSON() { return { type: this.constructor.name, ...this }; }
  describe() { return `${this.constructor.name}(${this.name}): area=${this.area().toFixed(2)} perim=${this.perimeter().toFixed(2)}`; }
  compareTo(o) { return this.area() - o.area(); }
}
const Scalable = Base => class extends Base { scale(f) { for (const k of this.constructor.dims) this[k] *= f; return this; } };
const Named = Base => class extends Base { get label() { return `<${this.name}>`; } };
const Circle = Shape.register(class Circle extends Named(Scalable(Shape)) { static dims = ['r']; constructor(name, r) { super(name); this.r = r; } area() { return Math.PI * this.r ** 2; } perimeter() { return 2 * Math.PI * this.r; } });
const Rect = Shape.register(class Rect extends Scalable(Shape) { static dims = ['w', 'h']; constructor(name, w, h) { super(name); this.w = w; this.h = h; } area() { return this.w * this.h; } perimeter() { return 2 * (this.w + this.h); } });
const Square = Shape.register(class Square extends Rect { constructor(name, s) { super(name, s, s); } describe() { return 'Square! ' + super.describe(); } });
const Tri = Shape.register(class Tri extends Shape { constructor(name, a, b, c) { super(name); Object.assign(this, { a, b, c }); } area() { const s = (this.a + this.b + this.c) / 2; return Math.sqrt(s * (s - this.a) * (s - this.b) * (s - this.c)); } perimeter() { return this.a + this.b + this.c; } });
const shapes = [new Circle('c1', 1), new Rect('r1', 2, 3), new Square('s1', 2), new Tri('t1', 3, 4, 5), new Circle('c2', 0.5).scale(4)];
for (const s of shapes.sort((a, b) => a.compareTo(b))) console.log(s.describe());
console.log(shapes.map(s => s instanceof Rect), shapes.filter(s => s instanceof Shape).length, shapes[0].label ?? 'no label', Square.dims, Object.getPrototypeOf(Square) === Rect);
const json = JSON.stringify(shapes); console.log(json);
const revived = JSON.parse(json).map(o => Shape.fromJSON(JSON.stringify(o))); console.log(revived.map(s => s.describe()).join('\n'));
try { new Shape('x') } catch (e) { console.log(e.name, e.message) }
try { Shape.fromJSON('{"type":"Hex"}') } catch (e) { console.log(e.name, e.message) }
class Weird extends Shape { constructor() { super('w'); } } try { new Weird().area() } catch (e) { console.log(e.message) }
console.log([...Shape.registry.keys()].join(), Circle.name, Object.getOwnPropertyNames(Circle.prototype), typeof Circle.prototype.scale);
const totalArea = shapes.reduce((s, x) => s + x.area(), 0); console.log(totalArea.toFixed(6));
class Node { constructor(v, ...kids) { this.v = v; this.kids = kids; } *walk(depth = 0) { yield [this.v, depth]; for (const k of this.kids) yield* k.walk(depth + 1); } get size() { return 1 + this.kids.reduce((s, k) => s + k.size, 0); } }
const tree = new Node('root', new Node('a', new Node('a1'), new Node('a2')), new Node('b', new Node('b1', new Node('b1x'))));
console.log([...tree.walk()].map(([v, d]) => '  '.repeat(d) + v).join('\n'), tree.size);
