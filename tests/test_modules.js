import { readFileSync, writeFileSync, existsSync } from 'fs';
import { join, dirname, basename } from 'path';

// Test fs module
writeFileSync('/tmp/cinder_test.txt', 'Hello, Cinder!');
const content = readFileSync('/tmp/cinder_test.txt', 'utf8');
console.log('fs.readFileSync:', content);
console.log('fs.existsSync:', existsSync('/tmp/cinder_test.txt'));

// Test path module
console.log('path.join:', join('/foo', 'bar', 'baz'));
console.log('path.dirname:', dirname('/foo/bar/baz.js'));
console.log('path.basename:', basename('/foo/bar/baz.js'));
