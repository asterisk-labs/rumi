// The Lake Annecy file of the landing page and the docs, written by
// tools/make_hero.py. Four Sentinel-2 bands of 1280 by 1280 pixels in tiles of
// 256, one band per frame, each frame with the smaller GPU recipe.

(function (rumi) {
  'use strict';

  rumi.annecy = {
    bands: ['B02', 'B03', 'B04', 'B08'],
    date: '2025-08-08',
    width: 1280,
    height: 1280,
    tile: 256,
    epsg: 32631,
    transform: [10.0, 0.0, 739760.0, 0.0, -10.0, 5087560.0],
    fileSize: 7274593,
    headerSize: 232,
    countMin: 38991,
    countBits: 16,
    recipes: { p: 'planar>zigzag>pfor', l: 'planar>zigzag>lanes' },
    // per band, row by row: the recipe each frame chose and its size in bytes
    frames: [
      ['lppllppllllplpllllllllllp', [80707, 67791, 68439, 64795, 71349, 70671, 68693, 60885, 73804, 67588, 62860, 71049, 74014, 67087, 67647, 65497, 68042, 72177, 69163, 64972, 71042, 64682, 68415, 72335, 67649]],
      ['lpplllppllllppllllppllllp', [80328, 67019, 67330, 71636, 74975, 73784, 69600, 56339, 75315, 74547, 70202, 74207, 72986, 65888, 73588, 72304, 72451, 74865, 69986, 67862, 73771, 70762, 73737, 73643, 70100]],
      ['lpplllpplllpppllplppllllp', [81212, 63687, 63360, 66295, 72493, 72294, 66880, 49955, 74637, 67944, 65445, 72151, 70874, 62536, 69461, 68622, 70145, 73529, 69259, 64342, 72844, 67336, 70506, 72888, 67056]],
      ['lpplllppllllppllllpplllll', [88496, 69075, 68195, 87511, 90256, 88112, 77684, 38991, 88261, 90858, 88390, 87262, 73684, 69260, 88620, 88128, 87093, 89176, 80347, 78004, 85744, 87351, 89048, 85671, 83347]]
    ]
  };
})((window.rumi = window.rumi || {}));
