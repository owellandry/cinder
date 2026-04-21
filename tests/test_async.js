async function greet(name) {
  return 'Hello, ' + name + '!';
}

greet('Cinder').then(msg => console.log(msg));

// Test promise chaining
Promise.resolve(42)
  .then(v => v * 2)
  .then(v => console.log('Promise result:', v));
