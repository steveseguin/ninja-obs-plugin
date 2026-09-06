const test = require('node:test');
const assert = require('node:assert/strict');
const vm = require('node:vm');
const { installApplicationDiagnostics } = require('./tools/application-diagnostics.cjs');
function fixture() {
  const calls=[],handlers={};
  const sandbox={console:{error(...args){calls.push({self:this,args});return 42;}},performance};
  sandbox.window=sandbox;sandbox.addEventListener=(name,handler)=>{handlers[name]=handler;};
  vm.createContext(sandbox);vm.runInContext(`(${installApplicationDiagnostics.toString()})()`,sandbox);
  return {sandbox,calls,handlers};
}
test('application diagnostics preserve console arguments, receiver and return value',()=>{
  const {sandbox,calls}=fixture();const receiver={},value={get stack(){throw new Error('getter');}};
  assert.equal(sandbox.console.error.call(receiver,value,'test'),42);
  assert.equal(calls[0].self,receiver);assert.equal(calls[0].args[0],value);
  assert.match(sandbox.__applicationErrors[0].message,/unprintable/);
});
test('browser errors retain stacks and remain bounded without suppressing console output',()=>{
  const {sandbox,calls,handlers}=fixture();
  handlers.error({error:{stack:'native stack'}});
  handlers.unhandledrejection({reason:'rejected'});
  for(let i=0;i<250;i++)sandbox.console.error(i);
  assert.equal(sandbox.__applicationErrors[0].message,'native stack');
  assert.equal(sandbox.__applicationErrors[1].kind,'unhandledrejection');
  assert.equal(sandbox.__applicationErrors.length,200);assert.equal(calls.length,250);
});
