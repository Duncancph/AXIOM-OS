// Runs a file as a classic script (like a browser <script>), not as a CommonJS module.
const vm = require('vm'), fs = require('fs'), path = require('path');
const file = process.argv[2];
vm.runInThisContext(fs.readFileSync(file, 'utf8'), { filename: path.basename(file) });
