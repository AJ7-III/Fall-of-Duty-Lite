// Native-only infantry art. Geometry, textiles, gait and hand shapes are
// built here. Weapon sockets provide the only input to the first-person rig.
import { Quaternion, Vector3 } from "@babylonjs/core/Maths/math.vector";
import { frameQuat } from "../reference/browser/src/anim/boneMath.ts";
import { BoltActionSniper } from "../reference/browser/src/weapons/BoltActionSniper.ts";

const v = (x, y, z) => new Vector3(x, y, z);
const up = v(0, 1, 0);
const forward = v(0, 0, 1);
const cross = Vector3.Cross;
const dot = Vector3.Dot;
const lerp = Vector3.Lerp;
const clamp = (n, low, high) => Math.max(low, Math.min(high, n));
const smooth = (t) => t * t * (3 - 2 * t);
const tau = 2 * Math.PI;

const fieldKit = {
  jacket: [[162, 135, 94], [124, 136, 95], true],
  pants: [[110, 102, 78], [83, 93, 67], true],
  helmet: [[108, 102, 74], [79, 91, 66], true],
  vest: [[89, 77, 58], [60, 71, 54], true],
  pocket: [[126, 109, 78], [96, 108, 78], true],
  stitching: [[142, 119, 83], [106, 116, 85], false],
  webbing: [[68, 59, 44], [47, 55, 46], true],
  boot: [[67, 54, 43], [49, 55, 59], false],
  sole: [[30, 30, 29], [30, 30, 29], false],
  gloves: [[87, 76, 59], [65, 72, 71], true],
  knuckle: [[111, 100, 80], [95, 105, 99], false],
  skin: [[184, 130, 101], [169, 115, 87], false],
  skinShadow: [[139, 89, 70], [123, 79, 60], false],
  strap: [[47, 39, 36], [47, 39, 36], false],
  hardware: [[115, 112, 96], [115, 112, 96], false],
  metal: [[57, 61, 64], [57, 61, 64], false],
  walnut: [[105, 66, 42], [105, 66, 42], true],
  accent: [[187, 155, 103], [158, 169, 124], false],
};

const hanzKit = {
  jacket: [137, 81, 73], pants: [76, 78, 82], helmet: [70, 70, 73], vest: [65, 61, 62],
  pocket: [108, 83, 78], stitching: [157, 116, 105], webbing: [66, 55, 56],
  gloves: [66, 65, 65], knuckle: [102, 92, 86], skin: [162, 112, 85], skinShadow: [121, 79, 64],
  accent: [173, 119, 101],
};

export function registerInfantryMaterials(palette, to565, base64) {
  const rival = {}, player = {}, hanz = {};
  const add = (name, channels, textile, camouflage = false) => {
    let texture = -1;
    const colorAt = (grade) => to565(...channels.map((c) => clamp(c * grade / 255, 0, 1)));
    if (textile) {
      const size = name.includes("gloves") || name.includes("jacket") ? 64 : 32;
      const pixels = new Uint16Array(size * size);
      for (let y = 0; y < size; y++) {
        for (let x = 0; x < size; x++) {
          const twill = ((x + y) % 4 - 1.5) * 0.012;
          const warp = Math.cos((x * 3 + y) * tau / size) * Math.sin(y * tau / size) * 0.045;
          const folds = 0.075 * Math.cos((x * 2 + 2 * Math.sin(y * tau / size)) * tau / size);
          const ripstop = x % 8 === 0 ? 0.026 : y % 8 === 0 ? 0.015 : 0;
          const u = x * tau / size, v = y * tau / size;
          const flecks = Math.cos(2 * u + v) + Math.sin(3 * v - u) + .50 * Math.sin(4 * u + 3 * v);
          const dye = camouflage ? clamp(flecks * .48, -.29, .06) : 0;
          pixels[y * size + x] = colorAt(1 + twill + warp + folds + ripstop + dye);
        }
      }
      texture = palette.textures.length;
      palette.textures.push({ name, width: size, height: size, pixels: base64(pixels) });
    }
    const id = palette.materials.length;
    palette.materials.push({ name, color: colorAt(1), texture, alpha: 255, doubleSided: true });
    return id;
  };
  for (const [part, [sand, olive, textile]] of Object.entries(fieldKit)) {
    rival[part] = add(`liteFieldSand_${part}`, sand, textile);
    const camouflage = part === "jacket" || part === "pants";
    player[part] = sand.every((c, i) => c === olive[i]) ? rival[part] : add(`liteFieldOlive_${part}`, olive, textile, camouflage);
    hanz[part] = hanzKit[part] ? add(`liteHanz_${part}`, hanzKit[part], textile) : rival[part];
  }
  return { rival, player, hanz };
}

export function registerInfantryFaces(palette, infantry, to565, base64) {
  const size = 64;
  const oval = (x, y, width, height) => clamp((1 - (x / width) ** 2 - (y / height) ** 2) * 3, 0, 1);
  const faces = [
    ["rival", fieldKit.skin[0], [54, 37, 28], 0, -.016],
    ["hanz", hanzKit.skin, [57, 68, 57], .61, .022],
    ["player", fieldKit.skin[1], [48, 45, 34], .19, .006],
  ];
  for (const [name, skin, iris, stubble, browTilt] of faces) {
    const pixels = new Uint16Array(size * size);
    for (let row = 0; row < size; row++) {
      for (let col = 0; col < size; col++) {
        const x = (col + .5) * 2 / size - 1, y = (row + .5) / size;
        const ax = Math.abs(x);
        const socket = oval(ax - .42, y - .590, .27, .105);
        const cheek = oval(ax - .59, y - .448, .23, .15);
        const nose = oval(x + .018, y - .474, .20, .20);
        const shade = 1.06 - .12 * ax ** 3 - .15 * socket + .07 * nose + .08 * cheek - .13 * (1 - y) ** 2;
        const grain = ((col * 17 + row * 31 + col * row * 7) % 9 - 4) * .55;
        const color = skin.map((c) => c * shade + grain);
        const pigment = (rgb, amount) => {
          for (let i = 0; i < color.length; i++) color[i] += (rgb[i] - color[i]) * amount;
        };
        const beard = clamp((.37 - y + .13 * ax) * 5, 0, 1);
        pigment([65, 49, 43], stubble * beard * (.66 + .34 * (grain < 0)));
        pigment([110, 58, 49], .47 * oval(ax - .115, y - .355, .061, .033));
        pigment([119, 56, 47], .58 * oval(x, y - .241, .315, .038));
        pigment([185, 103, 89], .41 * oval(x + .02, y - .207, .23, .041));

        const eyelid = y - .592 - (ax - .42) * .04;
        const eye = oval(ax - .42, eyelid, .196, .050);
        pigment([73, 43, 34], .83 * oval(ax - .42, eyelid - .011, .221, .067));
        pigment([191, 182, 162], eye);
        pigment(iris, eye * oval(ax - .397, eyelid + .004, .081, .064));
        pigment([34, 32, 29], eye * oval(ax - .397, eyelid + .004, .045, .044));
        pigment([224, 214, 189], .57 * eye * oval(ax - .365, eyelid - .015, .023, .020));
        const brow = y - .699 - (ax - .42) * browTilt * 9;
        pigment([61, 43, 33], .92 * oval(ax - .43, brow, .213, .038));
        pixels[row * size + col] = to565(...color.map((c) => c / 255));
      }
    }
    const texture = palette.textures.length;
    const label = `liteFace_${name}`;
    palette.textures.push({ name: label, width: size, height: size, pixels: base64(pixels) });
    infantry[name].face = palette.materials.length;
    palette.materials.push({ name: label, color: to565(...skin.map((c) => c / 255)), texture, alpha: 255, doubleSided: true });
  }
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
  loft(a, b, rings, material, reference = forward, sides = 10, panel) {
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
      for (let i = 0; i < sides; i++) {
        const color = panel?.(row - 1, i) ?? material;
        this.quad(rows[row - 1][i], rows[row][i], rows[row][i + 1], rows[row - 1][i + 1], color);
      }
    }
    for (const end of [0, rows.length - 1]) {
      const [t, , , shift = 0, offset] = rings[end];
      const center = lerp(a, b, t);
      if (offset) center.addInPlace(offset);
      const cap = this.vertex(point(center, axes, 0, 0, shift), 0.5, 0.5);
      const band = end === 0 ? 0 : end - 1;
      for (let i = 0; i < sides; i++) {
        this.triangle(rows[end][i], cap, rows[end][i + 1], panel?.(band, i) ?? material);
      }
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

const upperSleeve = [[-.12, .063, .061], [.13, .078, .071], [.40, .072, .065], [.78, .059, .054], [1.04, .052, .048]];
const lowerSleeve = [[-.10, .055, .050], [.19, .059, .053, .002], [.44, .052, .046, .005], [.74, .044, .040, -.001],
  [.87, .038, .034], [.895, .040, .037], [1.015, .038, .035]];
const viewSleeve = lowerSleeve.map(([t, width, depth, shift]) =>
  t < .8 ? [t, width * 1.42, depth * 1.30, shift] : [t, width * 1.18, depth * 1.14, shift]);
const thigh = [[-.12, .097, .088], [.23, .105, .094], [.58, .092, .087], [.89, .075, .071], [1.04, .067, .068]];
const shin = [[-.06, .069, .067], [.24, .075, .069], [.66, .062, .054], [1.06, .047, .046]];

function clothingArm(mesh, shoulder, elbow, wrist, mat, firstPerson = false) {
  mesh.loft(shoulder, elbow, upperSleeve, mat.jacket);
  mesh.loft(elbow, wrist, firstPerson ? viewSleeve : lowerSleeve, mat.jacket, forward, 10, (band, side) => {
    if (band >= 4) return band === 4 && side >= 6 && side <= 8 ? mat.stitching : mat.webbing;
    if (band >= 1 && side >= 5 && side <= 9) return mat.pocket;
  });
}

const fingerNames = ["index", "middle", "ring", "pinky"];
const fingerLengths = [[.033, .027, .019], [.037, .027, .020], [.034, .025, .018], [.024, .020, .015]];

function gloveDigit(mesh, joints, axes, width, mat) {
  const sides = 6;
  const first = joints[0], tip = joints.at(-1), beforeTip = joints.at(-2);
  const path = [lerp(first, joints[1], -.09), lerp(first, joints[1], .36),
    ...joints.slice(1, -1), lerp(beforeTip, tip, .85)];
  const rows = path.map((center, i) => {
    const tangent = (path[i + 1] ?? tip).subtract(path[i - 1] ?? center);
    const ring = basis(tangent.lengthSquared() > 1e-8 ? tangent : axes.y, axes.x);
    const radius = width * (i === 0 ? .82 : i === path.length - 1 ? .61 : 1 - (i - 1) * .12);
    return Array.from({ length: sides + 1 }, (_, side) => {
      const angle = side * tau / sides;
      return mesh.vertex(point(center, ring, Math.cos(angle) * radius * .80, 0, Math.sin(angle) * radius),
        side / sides, i / path.length);
    });
  });
  for (let i = 1; i < rows.length; i++) {
    for (let side = 0; side < sides; side++) {
      const material = i <= 2 && (side === 0 || side === sides - 1) ? mat.knuckle
        : (side === 2 || side === 3) && i >= 2 ? mat.webbing : mat.gloves;
      mesh.quad(rows[i - 1][side], rows[i][side], rows[i][side + 1], rows[i - 1][side + 1], material);
    }
  }
  const heel = mesh.vertex(path[0], .5, 0), end = mesh.vertex(tip, .5, 1);
  const last = rows.at(-1);
  for (let i = 0; i < sides; i++) {
    mesh.triangle(rows[0][i], heel, rows[0][i + 1], mat.gloves);
    mesh.triangle(last[i + 1], end, last[i], mat.gloves);
  }
}

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
  const padding = contact.padding ?? 0;
  const bulk = padding ? 1.20 : 1;
  // Shift the extra padding to the back of the glove, away from the grip surface.
  const palmRing = (at, width, depth, arc, pad) => [at, width * bulk, depth + padding * pad, -padding * pad, bow.scale(arc)];
  mesh.loft(wrist, knuckle, [[-.055, .028 * bulk, .015], palmRing(.19, .032, .017, .16, .22),
    palmRing(.43, .037, .018, .66, .64), palmRing(.64, .041, .018, 1, 1),
    palmRing(.83, .041, .017, .68, .83), palmRing(.98, .038, .014, .15, .27),
    [1.055, .035 * bulk, .014]], mat.gloves, axes.z, 10, (band, sector) => {
    if (band === 0 && sector >= 5) return mat.webbing;
    if (band >= 1 && band <= 4 && sector >= 1 && sector <= 3) return mat.webbing;
    if (band >= 2 && band <= 4 && sector >= 6 && sector <= 8) return mat.knuckle;
  });
  const thumbSide = side === "right" ? -1 : 1;
  for (let i = 0; i < 4; i++) {
    let joint = point(knuckle, axes, thumbSide * (.029 - .019 * i), [0, .004, 0, -.011][i], .001);
    let angle = 0;
    const amount = curl[fingerNames[i]];
    const tip = i === 0 ? contact.index : contact.fingers?.[i - 1];
    const from = joint;
    const joints = [joint];
    for (let j = 0; j < 3; j++) {
      angle += (Array.isArray(amount) ? amount[j] : amount * (1 - j * .16)) * 1.15;
      const t = [.43, .78, 1][j];
      const length = fingerLengths[i][j];
      const free = point(joint, axes, thumbSide * (.0012 - i * .0007), length * Math.cos(angle), length * Math.sin(angle));
      const arc = Math.sin(t * Math.PI);
      const target = tip && point(lerp(from, tip, t), axes, 0, .026 * arc, -.016 * arc);
      const bent = contact.wrap ? lerp(target, contact.wrap[i][j], contact.wrapWeight) : target;
      const reach = bent ? lerp(free, bent, contact.weight) : free;
      joints.push(reach);
      joint = reach;
    }
    gloveDigit(mesh, joints, axes, [.0102, .0107, .0101, .0085][i] * bulk, mat);
  }
  const thumbCurl = Array.isArray(curl.thumb) ? curl.thumb[0] : curl.thumb;
  const base = point(wrist, axes, thumbSide * .025, palmLength * .44, .007).addInPlace(bow.scale(.67));
  const freeMiddle = point(base, axes, thumbSide * .016, .027, .012);
  const middle = contact.thumbJoint ? lerp(freeMiddle, contact.thumbJoint, contact.seated) : freeMiddle;
  const freeThumb = point(middle, axes, -thumbSide * .010, .030, .011 + thumbCurl * .014);
  const tip = contact.thumb ? lerp(freeThumb, contact.thumb, contact.weight) : freeThumb;
  gloveDigit(mesh, [base, middle, tip], axes, .014 * bulk, mat);
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
    right: {
      wrist: [.052, .026, -.234], knuckle: [.045, .013, -.155], pad: [.026, .013, -.170],
      thumbJoint: [-.014, .043, -.203], thumb: [-.038, .068, -.151],
      wrap: [
        [[.042, .055, -.119], [.023, .049, -.091], [.009, .044, -.099]],
        [[.036, .012, -.123], [.014, .006, -.125], [-.015, .010, -.129]],
        [[.036, -.012, -.132], [.014, -.013, -.133], [-.012, -.010, -.137]],
        [[.034, -.038, -.142], [.014, -.039, -.143], [-.012, -.035, -.147]],
      ],
    },
    left: { wrist: [-.078, .011, .109], knuckle: [.002, .023, .108], pad: [-.015, .035, .107],
      index: [.027, .070, .136], fingers: [[.028, .068, .118], [.028, .063, .100], [.027, .055, .083]],
      thumbJoint: [-.034, .048, .144], thumb: [-.029, .066, .154] },
  },
  m40a3: {
    right: {
      wrist: [.060, .070, -.304], knuckle: [.045, .019, -.247], pad: [.026, .036, -.263],
      thumbJoint: [.027, .106, -.287], thumb: [-.017, .095, -.250],
      // The index bends into the guard. The other fingers turn under the stock behind it.
      wrap: [
        [[.042, .017, -.209], [.024, .005, -.193], [.009, .007, -.201]],
        [[.036, -.008, -.247], [.014, -.015, -.250], [-.007, -.009, -.252]],
        [[.034, -.008, -.262], [.010, -.011, -.265], [-.010, -.004, -.266]],
        [[.031, -.002, -.277], [.010, .001, -.282], [-.008, .010, -.282]],
      ],
    },
    left: { wrist: [-.081, -.054, .089], knuckle: [0, -.012, .111], pad: [-.013, .039, .111], index: [.040, .043, .135],
      fingers: [[.038, .041, .115], [.035, .041, .095], [.033, .035, .077]], thumb: [-.032, .045, .155] },
  },
  usp45: {
    right: {
      wrist: [.033, .068, -.160], knuckle: [.038, .046, -.076], pad: [.019, .053, -.080],
      thumbJoint: [-.025, .089, -.109], thumb: [-.028, .095, -.049],
      wrap: [
        [[.036, .070, -.027], [.023, .065, .001], [.008, .065, -.003]],
        [[.032, .046, -.036], [.012, .043, -.034], [-.012, .043, -.038]],
        [[.031, .020, -.045], [.011, .017, -.044], [-.012, .017, -.048]],
        [[.028, -.005, -.056], [.011, -.007, -.055], [-.011, -.005, -.057]],
      ],
    },
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
    const { knuckles, curl, toWorld, altBlend } = pose;
    let { palm } = pose;
    const rest = contacts[weapon.id][side];
    const sniperTrigger = weapon.id === "m40a3" && side === "right";
    const elapsed = weapon.reloadTotal - weapon.timer;
    const move = anchor.offset.length();
    const seated = (1 - altBlend) * (1 - clamp(move / .10, 0, 1));
    const socket = rest.wrist ? lerp(pose.wrist, toWorld(v(...rest.wrist)), seated) : pose.wrist;
    let wrist = socket.add(v(sign * .008, -.010, -.045).scale(seated));
    let boltGrip = sniperTrigger ? altBlend : 0;
    if (sniperTrigger && weapon.state === "reloading") {
      const { RELOAD_OPEN: open, RELOAD_PER_ROUND: feed, RELOAD_CLOSE: close } = BoltActionSniper;
      const closeStart = weapon.reloadTotal - close;
      const release = smooth(clamp((elapsed - open) / (feed * .19), 0, 1));
      const grab = smooth(clamp((elapsed - closeStart) / (close * .28), 0, 1));
      boltGrip *= 1 - release + grab;
      // Bring the feeding hand across the receiver once it comes up from the pouch.
      const reach = smooth(clamp((anchor.offset.y - .006) / .044, 0, 1)) * (1 - boltGrip);
      const across = Vector3.TransformNormal(v(-.06, 0, 0), anchor.node.parent.getWorldMatrix());
      wrist.addInPlace(across.scale(reach));
    }
    const boltMatrix = boltGrip > .001 ? model.pivots.boltGroup.computeWorldMatrix(true) : null;
    const bolt = (x, y, z) => Vector3.TransformCoordinates(v(x, y, z), boltMatrix);
    if (boltMatrix) {
      // Move the wrist with the pivot so the glove keeps its shape through the lift and pull.
      wrist = lerp(wrist, bolt(.140, .038, -.059), boltGrip);
      const inward = bolt(.055, 0, -.010).subtractInPlace(bolt(.088, .014, -.010)).normalize();
      palm = lerp(palm, inward, boltGrip).normalize();
    }
    const shoulder = v(.048 + sign * .183, -.424, .134);
    const elbow = bendJoint(shoulder, wrist, v(sign * .49, -.88, -.028), .282, .294);
    const length = side === "right" ? .149 - (sniperTrigger ? .053 * altBlend : 0) : .125;
    const free = wrist.add(knuckles.scale(length));
    const [index, ...fingers] = rest.wrap ? rest.wrap.map((joints) => joints[2]) : [rest.index, ...rest.fingers];
    const contact = {
      length, weight: seated, padding: side === "left" ? .017 : .013,
      knuckle: lerp(free, toWorld(v(...rest.knuckle)), seated),
      pad: toWorld(v(...rest.pad)),
      index: toWorld(v(...index)), thumb: toWorld(v(...rest.thumb)),
      fingers: fingers.map((p) => toWorld(v(...p))),
    };
    if (boltMatrix) {
      contact.knuckle = lerp(contact.knuckle, bolt(.088, .014, -.010), boltGrip);
      contact.pad = lerp(contact.pad, bolt(.066, .004, -.011), boltGrip);
      contact.index = lerp(contact.index, bolt(.055, -.011, -.005), boltGrip);
      contact.thumb = lerp(contact.thumb, bolt(.065, .011, -.020), boltGrip);
      contact.fingers = contact.fingers.map((p, i) => lerp(p, bolt(.059, -.010, -.013 - i * .009), boltGrip));
      contact.weight = Math.max(boltGrip, seated);
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
    if (rest.thumbJoint) {
      // Keep the stock contact while seated and release it during bolt or magazine work.
      const wrap = rest.wrap ?? [index, ...fingers].map(([x, y, z], i) => [
        [.022, .028, z], [.034, .046 - i * .002, z], [x, y, z],
      ]);
      contact.wrap = wrap.map((joints) => joints.map((p) => toWorld(v(...p))));
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
    clothingArm(mesh, shoulder, elbow, wrist, mat, true);
    hand(mesh, wrist, knuckles, palm, side, curl, mat, contact);
  }
  return mesh;
}

function boots(mesh, ankle, axes, mat) {
  const heel = point(ankle, axes, 0, -.055, -.060), toe = point(ankle, axes, 0, -.055, .176);
  mesh.loft(heel, toe, [[0, .044, .031], [.14, .060, .047], [.38, .065, .053],
    [.67, .064, .036, -.010], [.92, .056, .025, -.017], [1, .038, .021, -.017]],
    mat.boot, axes.y, 8, (band, side) => {
      if (band === 2 && (side === 1 || side === 2)) return mat.webbing;
    });
  mesh.box(point(ankle, axes, 0, -.111, .058), axes, .144, .028, .248, mat.sole, .37);
}

function faceShell(mesh, feature, mat, jaw) {
  // Heights, half-widths and front depths from nose or chin out to the temples.
  const contour = [
    [.055, jaw * .79, .074, .073, .070, .060, .026],
    [.078, jaw, .081, .082, .080, .070, .025],
    [.100, .064, .089, .087, .082, .072, .024],
    [.123, .071, .112, .098, .085, .071, .017],
    [.144, .076, .100, .088, .083, .070, .014],
    [.163, .077, .096, .082, .079, .073, .011],
    [.181, .077, .089, .089, .085, .073, .010],
    [.204, .075, .082, .084, .081, .067, .008],
    [.231, .066, .073, .076, .074, .060, .006],
  ];
  const columns = [0, .22, .48, .75, 1];
  const faceEdges = (columns.length - 1) * 2;
  const texel = (u) => (u * 63 + .5) / 64;
  const rows = contour.map(([y, half, ...depths]) => {
    const row = [];
    for (let i = -4; i <= 4; i++) {
      const x = Math.sign(i) * columns[Math.abs(i)];
      row.push(mesh.vertex(feature(half * x, y, depths[Math.abs(i)]), texel((x + 1) / 2), texel((y - .055) / .176)));
    }
    for (let i = 1; i < 8; i++) {
      const angle = i * Math.PI / 8;
      row.push(mesh.vertex(feature(half * Math.cos(angle), y, depths[4] - .081 * Math.sin(angle)), 0, 0));
    }
    return row;
  });
  for (let band = 1; band < rows.length; band++) {
    for (let i = 0; i < rows[band].length; i++) {
      const next = (i + 1) % rows[band].length;
      mesh.quad(rows[band - 1][next], rows[band][next], rows[band][i], rows[band - 1][i],
        i < faceEdges ? mat.face : mat.skin);
    }
  }
  for (const end of [0, rows.length - 1]) {
    const row = rows[end], center = mesh.vertex(feature(0, contour[end][0], 0), 0, 0);
    for (let i = 0; i < row.length; i++) {
      const next = (i + 1) % row.length;
      mesh.triangle(row[end ? next : i], center, row[end ? i : next], mat.skin);
    }
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
  mesh.loft(start, top, profile([[0, .059, .061], [.115, .055, .057], [.185, .050, .047],
    [.29, .049, .049, .011]]), mat.skinShadow, axes.z, 10, (band) => band === 0 ? mat.pocket : undefined);
  faceShell(mesh, feature, mat, isHanz ? .063 : isPlayer ? .052 : .057);
  if (isHanz) {
    mesh.loft(start, top, profile([[.715, .085, .084, -.003], [.80, .087, .084, -.005],
      [.91, .079, .074, -.010], [.99, .053, .049, -.013], [1.015, .029, .031, -.016]]), mat.helmet, axes.z, 14);
    mesh.loft(start, top, profile([[.704, .086, .086], [.762, .090, .089]]), mat.webbing, axes.z, 14);
    for (const sign of [-1, 1]) {
      mesh.box(feature(sign * .083, .193, -.006), axes, .028, .052, .059, mat.webbing, .38);
    }
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
  for (const sign of [-1, 1]) {
    mesh.loft(feature(sign * .073, .104, -.013), feature(sign * .077, .161, -.015),
      [[0, .007, .011], [.27, .012, .018], [.71, .012, .017], [1, .006, .011]], mat.skinShadow, axes.z, 8);
    if (!isHanz) {
      const strapTop = feature(sign * .079, .181, .033);
      const strapBottom = feature(sign * .061, .072, .044);
      mesh.loft(strapBottom, strapTop, [[0, .006, .007], [1, .008, .006]], mat.strap, axes.z, 5);
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
  const hips = v(.026 + sway * .3, .993 + bob, -.046);
  const neck = v(-.016 - sway * .4, 1.522 + bob, .051 + pace * .082);
  const axes = basis(neck.subtract(hips), forward);
  const onBody = (x, y, z) => point(hips, axes, x * build, y, z);
  const casing = (rings) => rings.map(([t, w, d]) => [t, w * build, d]);
  mesh.loft(onBody(0, -.100, 0), neck, casing([[0, .165, .105], [.28, .179, .121], [.63, .212, .133],
    [.83, .215, .122], [.98, .106, .077]]), mat.jacket, axes.z, 14);
  mesh.loft(onBody(0, -.137, -.015), onBody(0, .037, 0), [[0, .117, .102], [.41, .177, .123],
    [.83, .180, .116], [1, .163, .113]], mat.pants, axes.z, 12);
  mesh.loft(onBody(0, -.022, 0), onBody(0, .041, 0), [[0, .181, .122], [1, .181, .119]], mat.webbing, axes.z, 12);
  mesh.box(onBody(0, .008, .122), axes, .059, .043, .022, mat.hardware);
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
    const knee = bendJoint(hip, ankle, v(ankle.x, .48, 1), .433, .438);
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
