// Native-only infantry art. Geometry, textiles, gait and hand shapes are
// built here. Weapon sockets provide the only input to the first-person rig.
import { Quaternion, Vector3 } from "@babylonjs/core/Maths/math.vector";
import { frameQuat } from "../reference/browser/src/anim/boneMath.ts";

const v = (x, y, z) => new Vector3(x, y, z);
const up = v(0, 1, 0);
const forward = v(0, 0, 1);
const cross = Vector3.Cross;
const dot = Vector3.Dot;
const lerp = Vector3.Lerp;
const clamp = (n, low, high) => Math.max(low, Math.min(high, n));
const tau = 2 * Math.PI;

const fieldKit = {
  jacket: [[155, 134, 104], [112, 119, 129], true],
  pants: [[110, 94, 77], [74, 79, 88], true],
  helmet: [[103, 91, 76], [78, 85, 92], true],
  vest: [[81, 73, 63], [57, 65, 73], true],
  pocket: [[112, 97, 80], [79, 89, 100], true],
  stitching: [[132, 114, 90], [106, 114, 123], false],
  webbing: [[63, 55, 48], [46, 51, 59], true],
  boot: [[54, 45, 39], [48, 49, 53], false],
  sole: [[30, 30, 29], [30, 30, 29], false],
  gloves: [[79, 71, 61], [63, 66, 71], true],
  knuckle: [[104, 94, 81], [91, 94, 100], false],
  skin: [[184, 127, 94], [170, 113, 82], false],
  skinShadow: [[123, 78, 58], [115, 70, 52], false],
  brow: [[47, 39, 36], [47, 39, 36], false],
  eye: [[177, 163, 139], [177, 163, 139], false],
  metal: [[57, 61, 64], [57, 61, 64], false],
  walnut: [[105, 66, 42], [105, 66, 42], true],
  accent: [[186, 154, 105], [165, 174, 183], false],
};

const hanzKit = {
  jacket: [116, 81, 76], pants: [75, 76, 80], helmet: [58, 59, 63], vest: [67, 65, 67],
  pocket: [100, 88, 87], stitching: [146, 119, 111], webbing: [65, 58, 60],
  gloves: [62, 62, 64], knuckle: [105, 101, 96], skin: [161, 110, 83], skinShadow: [112, 73, 59],
  accent: [155, 111, 96],
};

export function registerInfantryMaterials(palette, to565, base64) {
  const rival = {}, player = {}, hanz = {};
  const add = (name, channels, textile) => {
    let texture = -1;
    const colorAt = (grade) => to565(...channels.map((c) => clamp(c * grade / 255, 0, 1)));
    if (textile) {
      const size = name.includes("gloves") || name.includes("jacket") ? 64 : 32;
      const pixels = new Uint16Array(size * size);
      for (let y = 0; y < size; y++) {
        for (let x = 0; x < size; x++) {
          const weave = ((x + y * 3) % 5 - 2) * 0.021;
          const heather = Math.cos((x * 3 + y) * tau / size) * Math.sin(y * tau / size) * 0.061;
          const crease = 0.055 * Math.cos((x * 2 + 2 * Math.sin(y * tau / size)) * tau / size);
          const ripstop = x % 8 === 0 || y % 8 === 0 ? 0.042 : 0;
          pixels[y * size + x] = colorAt(1 + weave + heather + crease + ripstop);
        }
      }
      texture = palette.textures.length;
      palette.textures.push({ name, width: size, height: size, pixels: base64(pixels) });
    }
    const id = palette.materials.length;
    palette.materials.push({ name, color: colorAt(1), texture, alpha: 255, doubleSided: true });
    return id;
  };
  for (const [part, [sand, slate, textile]] of Object.entries(fieldKit)) {
    rival[part] = add(`liteFieldSand_${part}`, sand, textile);
    player[part] = sand.every((c, i) => c === slate[i]) ? rival[part] : add(`liteFieldSlate_${part}`, slate, textile);
    hanz[part] = hanzKit[part] ? add(`liteHanz_${part}`, hanzKit[part], textile) : rival[part];
  }
  return { rival, player, hanz };
}

function basis(axis, reference) {
  const y = axis.normalizeToNew();
  const fallback = Math.abs(y.z) < 0.85 ? forward : up;
  const front = Math.abs(dot(y, reference)) < 0.97 ? reference : fallback;
  const x = cross(y, front).normalize();
  return { x, y, z: cross(x, y).normalize() };
}

function point(origin, axes, x, y, z) {
  return origin.add(axes.x.scale(x)).addInPlace(axes.y.scale(y)).addInPlace(axes.z.scale(z));
}

class Figure {
  vertices = [];
  triangles = [];

  vertex(position, u, v) {
    const index = this.vertices.length / 5;
    this.vertices.push(position.x, position.y, position.z, u, v);
    return index;
  }

  triangle(a, b, c, material) {
    this.triangles.push(a, b, c, material, -1);
  }

  quad(a, b, c, d, material) {
    this.triangle(a, b, c, material);
    this.triangle(a, c, d, material);
  }

  // Elliptical cross sections placed on a bone or the torso axis. Extra
  // rings form cuffs, rounded shoulders, knees and the profile of the helmet.
  loft(a, b, rings, material, reference = forward, sides = 10) {
    const axes = basis(b.subtract(a), reference);
    const rows = [];
    for (const [t, width, depth, shift = 0, offset] of rings) {
      const center = lerp(a, b, t);
      if (offset) center.addInPlace(offset);
      const row = [];
      for (let i = 0; i <= sides; i++) {
        const angle = i * tau / sides;
        row.push(this.vertex(point(center, axes, Math.cos(angle) * width, 0, Math.sin(angle) * depth + shift), i / sides, t));
      }
      rows.push(row);
    }
    for (let row = 1; row < rows.length; row++) {
      for (let i = 0; i < sides; i++) this.quad(rows[row - 1][i], rows[row][i], rows[row][i + 1], rows[row - 1][i + 1], material);
    }
    for (const end of [0, rows.length - 1]) {
      const [t, , , shift = 0, offset] = rings[end];
      const center = lerp(a, b, t);
      if (offset) center.addInPlace(offset);
      const cap = this.vertex(point(center, axes, 0, 0, shift), 0.5, 0.5);
      for (let i = 0; i < sides; i++) this.triangle(rows[end][i], cap, rows[end][i + 1], material);
    }
  }

  // Chamfered sections give fabric pouches and boot toes softer corners
  // than cubes, without requiring a normal-map pass.
  box(center, axes, width, height, depth, material, bevel = 0.16) {
    const rings = [];
    for (const face of [-1, 1]) {
      const row = [];
      const w = width / 2, h = height / 2;
      const wx = w * (1 - bevel), hy = h * (1 - bevel);
      const corners = [[-wx, -h], [wx, -h], [w, -hy], [w, hy], [wx, h], [-wx, h], [-w, hy], [-w, -hy]];
      for (const [x, y] of corners) row.push(this.vertex(point(center, axes, x, y, face * depth / 2), x / width + .5, y / height + .5));
      const mid = this.vertex(point(center, axes, 0, 0, face * depth / 2), .5, .5);
      for (let i = 0; i < 8; i++) this.triangle(mid, row[i], row[(i + 1) % 8], material);
      rings.push(row);
    }
    for (let i = 0; i < 8; i++) this.quad(rings[0][i], rings[1][i], rings[1][(i + 1) % 8], rings[0][(i + 1) % 8], material);
  }
}

function bendJoint(start, end, pole, upper, lower) {
  const ray = end.subtract(start);
  const distance = Math.max(ray.length(), 0.001);
  const axis = ray.scale(1 / distance);
  const delta = pole.subtract(start);
  const side = delta.subtract(axis.scale(dot(delta, axis))).normalize();
  const span = clamp(distance, Math.abs(upper - lower) + .001, upper + lower - .001);
  const along = (upper * upper + span * span - lower * lower) / (2 * span);
  const bend = Math.sqrt(Math.max(0, upper * upper - along * along));
  return start.add(axis.scale(along)).addInPlace(side.scale(bend));
}

const upperSleeve = [[-.12, .062, .067], [.12, .079, .075], [.41, .074, .067], [.82, .059, .056], [1.06, .050, .048]];
const lowerSleeve = [[-.08, .053, .050], [.20, .060, .053], [.53, .049, .045], [.91, .037, .035], [1, .035, .033]];
const cuff = [[.84, .038, .036], [1.01, .039, .036]];
const thigh = [[-.12, .097, .088], [.23, .105, .094], [.58, .092, .087], [.89, .075, .071], [1.04, .067, .068]];
const shin = [[-.06, .069, .067], [.24, .075, .069], [.66, .062, .054], [1.06, .047, .046]];

function clothingArm(mesh, shoulder, elbow, wrist, mat) {
  mesh.loft(shoulder, elbow, upperSleeve, mat.jacket);
  mesh.loft(elbow, wrist, lowerSleeve, mat.jacket);
  mesh.loft(elbow, wrist, cuff, mat.webbing);
}

const fingerNames = ["index", "middle", "ring", "pinky"];
const fingerLengths = [[.033, .027, .019], [.037, .027, .020], [.034, .025, .018], [.024, .020, .015]];

function hand(mesh, wrist, knuckles, palm, side, curl, mat, contact = {}) {
  const length = contact.length ?? .094;
  const knuckle = contact.knuckle ?? wrist.add(knuckles.scale(length));
  const gripAxis = knuckle.subtract(wrist);
  const axes = basis(gripAxis, palm);
  const palmLength = gripAxis.length();
  // The inner pad meets the stock. Shape the intervening rings around it
  // so the wrist can approach at an angle without a straight palm bridging air.
  const middlePalm = lerp(wrist, knuckle, .64);
  const bow = contact.pad
    ? contact.pad.subtract(axes.z.scale(.017)).subtractInPlace(middlePalm).scaleInPlace(contact.weight)
    : Vector3.Zero();
  mesh.loft(wrist, knuckle, [[-.035, .025, .013], [.20, .033, .016, 0, bow.scale(.16)],
    [.43, .039, .018, 0, bow.scale(.66)], [.64, .041, .018, 0, bow],
    [.81, .041, .017, 0, bow.scale(.68)], [.96, .038, .015, 0, bow.scale(.16)],
    [1.04, .036, .014]], mat.gloves, axes.z, 8);
  if (!contact.wrap) {
    mesh.box(point(wrist, axes, 0, palmLength * .73, -.017).addInPlace(bow.scale(.81)),
      axes, .065, .044, .009, mat.knuckle, .30);
  }
  const segment = (start, end, radius, material) =>
    mesh.loft(start, end, [[-.06, radius * .78, radius * .71], [.21, radius, radius * .84],
      [.93, radius * .86, radius * .77], [1.07, radius * .64, radius * .63]], material, axes.x, 5);
  const thumbSide = side === "right" ? -1 : 1;
  for (let i = 0; i < 4; i++) {
    let joint = point(knuckle, axes, thumbSide * (.029 - .019 * i), [0, .004, 0, -.011][i], .001);
    let angle = 0;
    const amount = curl[fingerNames[i]];
    const tip = i === 0 ? contact.index : contact.fingers?.[i - 1];
    const from = joint;
    for (let j = 0; j < 3; j++) {
      angle += (Array.isArray(amount) ? amount[j] : amount * (1 - j * .16)) * 1.15;
      const t = [.43, .78, 1][j];
      const length = fingerLengths[i][j];
      const free = point(joint, axes, thumbSide * (.0012 - i * .0007), length * Math.cos(angle), length * Math.sin(angle));
      const arc = Math.sin(t * Math.PI);
      const target = tip && point(lerp(from, tip, t), axes, 0, .026 * arc, -.016 * arc);
      const bent = contact.wrap ? lerp(target, contact.wrap[i][j], contact.wrapWeight) : target;
      const reach = bent ? lerp(free, bent, contact.weight) : free;
      segment(joint, reach, (i === 3 ? .009 : .010) - j * .0011, mat.gloves);
      joint = reach;
    }
  }
  const thumbCurl = Array.isArray(curl.thumb) ? curl.thumb[0] : curl.thumb;
  const base = point(wrist, axes, thumbSide * .025, palmLength * .44, .007).addInPlace(bow.scale(.67));
  const freeMiddle = point(base, axes, thumbSide * .016, .027, .012);
  const middle = contact.thumbJoint ? lerp(freeMiddle, contact.thumbJoint, contact.seated) : freeMiddle;
  const freeThumb = point(middle, axes, -thumbSide * .010, .030, .011 + thumbCurl * .014);
  const tip = contact.thumb ? lerp(freeThumb, contact.thumb, contact.weight) : freeThumb;
  segment(base, middle, .015, mat.gloves);
  segment(middle, tip, .011, mat.gloves);
}

function grip(anchor) {
  const { pose, alt, altBlend, offset, node } = anchor;
  const t = alt ? altBlend : 0;
  const get = (xyz) => v(...xyz);
  const wrist = lerp(get(pose.wrist), get(alt?.wrist ?? pose.wrist), t).addInPlace(offset);
  const a = frameQuat(get(pose.knuckles), get(pose.palm), new Quaternion());
  const b = alt ? frameQuat(get(alt.knuckles), get(alt.palm), new Quaternion()) : a;
  const rotation = Quaternion.Slerp(a, b, t);
  const matrix = node.parent.computeWorldMatrix(true);
  const direction = (d) => Vector3.TransformNormal(d.applyRotationQuaternion(rotation), matrix).normalize();
  const curl = {};
  for (const name of ["thumb", ...fingerNames]) {
    const values = (value) => Array.isArray(value) ? value : [value, value * .84, value * .68];
    const from = values(pose.curl[name]), to = values(alt?.curl[name] ?? pose.curl[name]);
    curl[name] = from.map((n, i) => n + (to[i] - n) * t);
  }
  const toWorld = (p) => Vector3.TransformCoordinates(p, matrix);
  return { wrist: toWorld(wrist), knuckles: direction(up), palm: direction(forward), curl, toWorld, altBlend: t };
}

// Contact points in the weapon builders' local space. Wrist sockets and
// magazine/bolt excursions come from ViewModelRig.
const contacts = {
  mp44: {
    right: { knuckle: [.050, .005, -.154], pad: [.026, .013, -.170], index: [.010, .051, -.106], fingers: [[.006, .016, -.134], [.004, -.010, -.146], [.001, -.034, -.158]], thumb: [.007, .047, -.193] },
    left: { wrist: [-.078, .011, .109], knuckle: [.002, .023, .108], pad: [-.015, .035, .107],
      index: [.027, .070, .136], fingers: [[.028, .068, .118], [.028, .063, .100], [.027, .055, .083]],
      thumbJoint: [-.034, .048, .144], thumb: [-.029, .066, .154] },
  },
  m40a3: {
    right: { knuckle: [.042, .012, -.237], pad: [.024, .040, -.252], index: [.009, .006, -.203], fingers: [[.004, -.013, -.241], [.002, -.033, -.252], [-.003, -.050, -.267]], thumb: [.005, .045, -.279] },
    left: { knuckle: [0, -.012, .111], pad: [-.013, .039, .111], index: [.040, .043, .135], fingers: [[.038, .041, .115], [.035, .041, .095], [.033, .035, .077]], thumb: [-.032, .045, .155] },
  },
  usp45: {
    right: { knuckle: [.033, .041, -.073], pad: [.016, .037, -.080], index: [.007, .064, -.007], fingers: [[.000, .053, -.041], [-.002, .028, -.048], [-.002, .004, -.058]], thumb: [.009, .090, -.086] },
    left: { knuckle: [-.041, .026, -.071], pad: [-.025, .026, -.067], index: [.033, .060, -.040], fingers: [[.036, .039, -.047], [.035, .019, -.054], [.032, .001, -.060]], thumb: [-.026, .092, -.040] },
  },
};

function fitPalm(weaponMesh, hint, palm) {
  const { vertices, triangles } = weaponMesh;
  const direction = palm.normalizeToNew();
  const origin = hint.subtract(direction.scale(.039));
  const a = new Vector3(), b = new Vector3(), c = new Vector3();
  const load = (index, p) => p.set(vertices[index * 5], vertices[index * 5 + 1], vertices[index * 5 + 2]);
  const reach = .082;
  let distance = reach;
  for (let i = 0; i < triangles.length; i += 5) {
    load(triangles[i], a);
    load(triangles[i + 1], b);
    load(triangles[i + 2], c);
    const edge = b.subtract(a), other = c.subtract(a);
    const normal = cross(direction, other);
    const determinant = dot(edge, normal);
    if (Math.abs(determinant) < 1e-8) continue;
    const delta = origin.subtract(a);
    const u = dot(delta, normal) / determinant;
    if (u < 0 || u > 1) continue;
    const q = cross(delta, edge);
    const w = dot(direction, q) / determinant;
    if (w < 0 || u + w > 1) continue;
    const t = dot(other, q) / determinant;
    if (t >= 0 && t < distance) distance = t;
  }
  if (distance < reach) return origin.add(direction.scale(distance));

  // A palm approaching the edge of the other glove can miss the projection
  // ray. Find its nearest surface within the same local region.
  let nearest = hint, minSquared = .04 * .04;
  const accept = (p) => {
    const squared = Vector3.DistanceSquared(p, hint);
    if (squared < minSquared) {
      minSquared = squared;
      nearest = p;
    }
  };
  const edgePoint = (a, b) => {
    const edge = b.subtract(a);
    const size = edge.lengthSquared();
    if (size > 1e-12) accept(a.add(edge.scale(clamp(dot(hint.subtract(a), edge) / size, 0, 1))));
  };
  for (let i = 0; i < triangles.length; i += 5) {
    load(triangles[i], a);
    load(triangles[i + 1], b);
    load(triangles[i + 2], c);
    const ab = b.subtract(a), ac = c.subtract(a), ap = hint.subtract(a);
    const aa = dot(ab, ab), bb = dot(ac, ac), abac = dot(ab, ac);
    const determinant = aa * bb - abac * abac;
    if (determinant > 1e-12) {
      const u = (bb * dot(ap, ab) - abac * dot(ap, ac)) / determinant;
      const w = (aa * dot(ap, ac) - abac * dot(ap, ab)) / determinant;
      if (u >= 0 && w >= 0 && u + w <= 1) {
        accept(a.add(ab.scale(u)).addInPlace(ac.scale(w)));
        continue;
      }
    }
    edgePoint(a, b);
    edgePoint(b, c);
    edgePoint(c, a);
  }
  return nearest;
}

export function buildViewArms(weapon, model, mat, weaponMesh) {
  const mesh = new Figure();
  for (const side of ["right", "left"]) {
    const sign = side === "left" ? -1 : 1;
    const anchor = model.hands[side];
    const pose = grip(anchor);
    const { knuckles, palm, curl, toWorld, altBlend } = pose;
    const rest = contacts[weapon.id][side];
    const move = anchor.offset.length();
    const seated = (1 - altBlend) * (1 - clamp(move / .10, 0, 1));
    const wrist = rest.wrist ? lerp(pose.wrist, toWorld(v(...rest.wrist)), seated) : pose.wrist;
    const shoulder = v(.048 + sign * .183, -.431, .246);
    const elbow = bendJoint(shoulder, wrist, v(sign * .49, -.88, .12), .282, .294);
    const length = side === "right" ? .149 : .125;
    const free = wrist.add(knuckles.scale(length));
    const contact = {
      length, weight: seated,
      knuckle: lerp(free, toWorld(v(...rest.knuckle)), seated),
      pad: toWorld(v(...rest.pad)),
      index: toWorld(v(...rest.index)), thumb: toWorld(v(...rest.thumb)),
      fingers: rest.fingers.map((p) => toWorld(v(...p))),
    };
    const elapsed = weapon.reloadTotal - weapon.timer;
    const loadingRounds = weapon.state === "reloading" && elapsed >= .41 && elapsed < weapon.reloadTotal - .45;
    if (weapon.id === "m40a3" && side === "right" && altBlend > .001 && !loadingRounds) {
      const matrix = model.pivots.boltGroup.computeWorldMatrix(true);
      const bolt = (x, y, z) => Vector3.TransformCoordinates(v(x, y, z), matrix);
      contact.knuckle = lerp(contact.knuckle, bolt(.088, .014, -.010), altBlend);
      contact.pad = lerp(contact.pad, bolt(.066, .004, -.011), altBlend);
      contact.index = lerp(contact.index, bolt(.055, -.011, -.005), altBlend);
      contact.thumb = lerp(contact.thumb, bolt(.065, .011, -.020), altBlend);
      contact.fingers = contact.fingers.map((p, i) => lerp(p, bolt(.059, -.010, -.013 - i * .009), altBlend));
      contact.weight = Math.max(altBlend, seated);
    }
    const mag = model.pivots.magGroup;
    const rifleMag = weapon.id === "mp44";
    const seatedAt = rifleMag ? .92 : .75;
    if (side === "left" && mag && mag.isEnabled() && weapon.state === "reloading" && elapsed > .06 && elapsed < seatedAt + .08) {
      const y = rifleMag ? -.132 : .005, z = rifleMag ? .040 : .004;
      const matrix = mag.computeWorldMatrix(true);
      const contactAt = (x, dy, dz) => Vector3.TransformCoordinates(v(x, y + dy, z + dz), matrix);
      const proximity = 1 - clamp((Vector3.Distance(wrist, contactAt(-.031, 0, 0)) - .16) / .12, 0, 1);
      const weight = proximity * clamp((elapsed - .06) / .12, 0, 1) * clamp((seatedAt + .08 - elapsed) / .12, 0, 1);
      contact.knuckle = lerp(contact.knuckle, contactAt(-.032, -.014, 0), weight);
      contact.pad = lerp(contact.pad, contactAt(-.026, -.002, -.003), weight);
      contact.index = lerp(contact.index, contactAt(.024, .013, .024), weight);
      contact.thumb = lerp(contact.thumb, contactAt(-.017, .030, -.023), weight);
      contact.fingers = contact.fingers.map((p, i) => lerp(p, contactAt(.023, .004 - i * .014, .019), weight));
      contact.weight = Math.max(weight, seated);
    }
    if (rest.wrist) {
      // Curl the four fingertips around the rounded wood and keep the thumb on
      // its near side. Blend the same joints back to the magazine pose in reload.
      contact.wrap = [rest.index, ...rest.fingers].map(([x, y, z], i) => [
        toWorld(v(.022, .028, z)), toWorld(v(.034, .046 - i * .002, z)), toWorld(v(x, y, z)),
      ]);
      contact.wrapWeight = clamp(seated / Math.max(contact.weight, .001), 0, 1);
      contact.seated = seated;
      contact.thumbJoint = toWorld(v(...rest.thumbJoint));
    }
    // A steady USP support palm seats on the firing glove. Other seated
    // grips project to the sampled weapon, including moving magazine and bolt.
    const restsOnGlove = weapon.id === "usp45" && side === "left" && seated > .85;
    if (contact.weight > .05) {
      const inward = basis(contact.knuckle.subtract(wrist), palm).z;
      contact.pad = fitPalm(restsOnGlove ? mesh : weaponMesh, contact.pad, inward);
    }
    clothingArm(mesh, shoulder, elbow, wrist, mat);
    hand(mesh, wrist, knuckles, palm, side, curl, mat, contact);
  }
  return mesh;
}

function boots(mesh, ankle, axes, mat) {
  mesh.box(point(ankle, axes, 0, -.053, .055), axes, .135, .116, .234, mat.boot, .33);
  mesh.box(point(ankle, axes, 0, -.111, .058), axes, .144, .028, .248, mat.sole, .32);
  for (let i = 0; i < 3; i++) {
    mesh.box(point(ankle, axes, 0, .003, .04 + i * .025), axes, .065, .008, .012, mat.stitching);
  }
}

function head(mesh, neck, axes, mat, identity) {
  const isHanz = identity === "hanz", isPlayer = identity === "player";
  const width = isHanz ? 1.31 : isPlayer ? 1.18 : 1.23;
  const depth = isHanz ? 1.20 : 1.16, height = isHanz ? 1.03 : 1.08;
  const feature = (x, y, z) => point(neck, axes, x * width, y * height, z * depth);
  const profile = (rings) => rings.map(([t, w, d, shift = 0]) => [t, w * width, d * depth, shift * depth]);
  const start = point(neck, axes, 0, -.018, .006);
  const top = feature(0, .284, .014);
  mesh.loft(start, top, profile([[0, .050, .050], [.29, .056, .055]]), mat.skinShadow, axes.z);
  const jaw = isHanz ? .066 : isPlayer ? .049 : .056;
  mesh.loft(start, top, profile([[.22, jaw, .050, .027], [.36, jaw + .008, .063, .021], [.52, .077, .072, .016],
    [.59, isPlayer ? .075 : .079, .073, .013], [.70, .078, .072, .007], [.89, .065, .061, -.008]]), mat.skin, axes.z, 14);
  if (isHanz) {
    mesh.loft(start, top, profile([[.715, .085, .084, -.003], [.80, .087, .084, -.005],
      [.91, .079, .074, -.010], [.99, .053, .049, -.013], [1.015, .029, .031, -.016]]), mat.helmet, axes.z, 14);
    mesh.loft(start, top, profile([[.704, .086, .086], [.762, .090, .089]]), mat.webbing, axes.z, 14);
    mesh.loft(start, top, profile([[.19, .048, .051, .027], [.28, .073, .070, .026],
      [.42, .075, .068, .021]]), mat.brow, axes.z, 12);
  } else {
    mesh.loft(start, top, profile([[.68, .085, .083, -.003], [.77, .093, .093, -.005],
      [.87, .092, .083, -.011], [.94, .080, .074, -.012], [1.035, .045, .046, -.013]]), mat.helmet, axes.z, 14);
    mesh.loft(start, top, profile([[.675, .089, .086, -.003], [.713, .094, .090, -.004]]), mat.webbing, axes.z, 14);
    if (isPlayer) {
      mesh.box(feature(0, .220, .093), axes, .156, .045, .026, mat.knuckle, .30);
      for (const sign of [-1, 1]) mesh.box(feature(sign * .030, .220, .108), axes, .056, .028, .012, mat.metal, .28);
    } else {
      mesh.box(feature(0, .223, .091), axes, .054, .041, .014, mat.webbing);
    }
  }
  mesh.box(feature(0, .132, .100), axes, isHanz ? .036 : .029, .042, .033, mat.skin);
  mesh.box(feature(0, .089, .080), axes, isHanz ? .055 : .047, .010, .013, mat.skinShadow);
  for (const sign of [-1, 1]) {
    mesh.box(feature(sign * .032, .165, .083), axes, .041, .012, .015, mat.brow);
    mesh.box(feature(sign * .034, .156, .087), axes, .033, .009, .013, mat.eye);
    mesh.box(feature(sign * .033, .156, .094), axes, .012, .010, .005, mat.brow);
    mesh.box(feature(sign * .075, .119, .000), axes, .024, .052, .044, mat.skinShadow, .37);
    if (!isHanz) {
      const strapTop = feature(sign * .079, .181, .033);
      const strapBottom = feature(sign * .061, .072, .044);
      mesh.loft(strapBottom, strapTop, [[0, .010, .010], [1, .009, .008]], mat.webbing, axes.z, 5);
    }
  }
}

export function buildFieldInfantry(gait, phase, mat, identity) {
  const mesh = new Figure();
  const isHanz = identity === "hanz", isPlayer = identity === "player";
  const build = isHanz ? 1.085 : isPlayer ? .986 : 1;
  const stride = gait === "run" ? .29 : gait === "walk" ? .19 : 0;
  const cycle = phase * tau, pace = stride / .29;
  const sway = (stride ? Math.sin(cycle) : .2) * .037;
  const bob = stride ? .027 * Math.cos(cycle * 2) : .003;
  const hips = v(.026 + sway * .3, .958 + bob, -.046);
  const neck = v(-.016 - sway * .4, 1.472 + bob, .051 + pace * .082);
  const axes = basis(neck.subtract(hips), forward);
  const onBody = (x, y, z) => point(hips, axes, x * build, y, z);
  const casing = (rings) => rings.map(([t, w, d]) => [t, w * build, d]);
  mesh.loft(onBody(0, -.100, 0), neck, casing([[0, .165, .105], [.28, .179, .121], [.63, .212, .133],
    [.83, .215, .122], [.98, .106, .077]]), mat.jacket, axes.z, 14);
  mesh.loft(onBody(0, -.137, -.015), onBody(0, .037, 0), [[0, .117, .102], [.41, .177, .123],
    [.83, .180, .116], [1, .163, .113]], mat.pants, axes.z, 12);
  mesh.loft(onBody(0, -.022, 0), onBody(0, .041, 0), [[0, .181, .122], [1, .181, .119]], mat.webbing, axes.z, 12);
  mesh.box(onBody(0, .008, .122), axes, .059, .043, .022, mat.metal);
  const plate = (x, y, z, w, h, d, material) => mesh.box(onBody(x, y, z), axes, w, h, d, material, .20);
  plate(0, .282, .099, (isHanz ? .297 : .364) * build, .300, .118, mat.vest);
  plate(0, .285, -.100, .324, .278, .109, mat.vest);
  plate(0, .247, -.169, .246, .235, .086, mat.pocket);
  plate(0, .354, -.222, .219, .049, .025, mat.webbing);
  for (const sign of [-1, 1]) {
    plate(sign * .127, .453, -.014, .05, .044, .213, mat.webbing);
    plate(sign * .161, .201, .118, .028, .226, .019, mat.webbing);
  }
  for (const x of isHanz ? [-.105, .075] : [-.114, 0, .114]) {
    const width = isHanz ? .120 : .093;
    plate(x, .211, .177, width, .152, .052, mat.pocket);
    plate(x, .276, .206, width * .97, .026, .016, mat.webbing);
    plate(x, .222, .206, .017, .057, .010, mat.stitching);
    plate(x, .198, .211, width * .79, .009, .008, mat.stitching);
  }
  plate(-.092, .347, .164, .111, .024, .016, mat.accent);
  if (isHanz) {
    plate(.167, .335, .122, .057, .122, .064, mat.metal);
    mesh.loft(onBody(.165, .382, .140), onBody(.159, .533, .128), [[0, .006, .006], [1, .004, .004]], mat.metal, axes.z, 5);
  } else if (isPlayer) {
    plate(.121, .318, .169, .091, .076, .030, mat.pocket);
    plate(.121, .339, .189, .068, .015, .011, mat.accent);
  }
  plate(-.192, .045, -.018, .077, .137, .123, mat.pocket);
  plate(.193, .058, -.019, .076, .142, .120, mat.webbing);
  head(mesh, neck, basis(v(sway * .2, 1, pace * .08), forward), mat, identity);

  const feet = basis(up, forward);
  for (const sign of [-1, 1]) {
    const step = cycle + (sign === 1 ? Math.PI : 0);
    const hip = onBody(sign * .113, -.067, 0);
    const lift = stride ? Math.max(0, Math.sin(step)) * (.074 + .036 * pace) : 0;
    const ankle = v(.026 + sign * .129, .125 + lift, .022 - sign * .166 + Math.cos(step) * stride);
    const knee = bendJoint(hip, ankle, v(ankle.x, .48, 1), .415, .418);
    mesh.loft(hip, knee, thigh, mat.pants);
    mesh.loft(knee, ankle, shin, mat.pants);
    const kneeAxes = basis(knee.subtract(ankle), forward);
    mesh.box(point(knee, kneeAxes, 0, -.036, .073), kneeAxes, .119, .119, .042, mat.helmet, .37);
    mesh.box(point(lerp(hip, knee, .48), axes, sign * .082, 0, -.010), axes, .043, .141, .117, mat.pocket);
    boots(mesh, ankle, feet, mat);
  }

  const carbine = basis(v(.054, -.223, 1), up);
  const rifleAxes = { x: carbine.x.negate(), y: carbine.z, z: carbine.y };
  const receiver = onBody(.079, .292, .314);
  const rifle = (x, y, z, w, h, d, material) => mesh.box(point(receiver, rifleAxes, x, y, z), rifleAxes, w, h, d, material);
  rifle(0, 0, -.02, .065, .078, .244, mat.metal);
  rifle(0, -.014, -.217, .061, .093, .191, mat.walnut);
  rifle(0, -.019, .160, .067, .065, .151, mat.walnut);
  rifle(0, .012, .271, .028, .033, .136, mat.metal);
  rifle(0, -.095, .039, .035, .162, .063, mat.metal);
  rifle(0, -.087, -.114, .042, .103, .058, mat.walnut);
  rifle(0, -.061, -.079, .017, .014, .052, mat.metal);
  rifle(0, -.044, -.062, .014, .036, .012, mat.metal);
  rifle(0, .061, .207, .031, .043, .030, mat.metal);
  const curl = { thumb: .6, index: .46, middle: .68, ring: .78, pinky: .84 };
  for (const side of ["left", "right"]) {
    const support = side === "left";
    const sign = support ? -1 : 1;
    const atGrip = (x, y, z) => point(receiver, rifleAxes, x, y, z);
    const shoulder = onBody(sign * .191, .412, -.005);
    const wrist = support ? atGrip(-.101, -.092, .114) : atGrip(.061, -.113, -.190);
    const elbow = bendJoint(shoulder, wrist, onBody(sign * .365, -.095, -.13), .272, .27);
    clothingArm(mesh, shoulder, elbow, wrist, mat);
    mesh.box(point(lerp(shoulder, elbow, .22), axes, 0, 0, .054), axes, .071, .055, .012, mat.pocket);
    const contact = support
      ? { knuckle: atGrip(-.012, -.054, .159), pad: atGrip(-.012, -.051, .159), index: atGrip(.034, -.004, .186), thumb: atGrip(-.037, -.005, .196),
        fingers: [.164, .148, .134].map((z) => atGrip(.034, -.004, z)), weight: 1 }
      : { knuckle: atGrip(.034, -.082, -.111), pad: atGrip(.021, -.087, -.117), index: atGrip(.010, -.045, -.067), thumb: atGrip(-.008, -.040, -.148),
        fingers: [-.069, -.094, -.112].map((y) => atGrip(-.004, y, -.080)), weight: 1 };
    hand(mesh, wrist, rifleAxes.z, support ? rifleAxes.y : rifleAxes.x.negate(), side, curl, mat, contact);
  }
  return mesh;
}
