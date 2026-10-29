/**
 * @file test_lisp_lib.c
 * @brief Unit tests for the Lisp library's list procedures (map, filter,
 * assoc, sort, ...), which lisp_lib.h implements in C.
 */

#include "test.h"
#include "../include/lisp.h"
#include "../include/lisp_lib.h"

static LispContext ctx;

// Evaluates `expr` and checks what (write) would print for the result.
static void checkEval(const char *expr, const char *expected) {
  LispError error = LISP_ERROR_NONE;
  FILE *out = tmpfile();
  char printed[1024];
  size_t n;
  Lisp result;

  result = lisp_eval(lisp_read(expr, &error, ctx), &error, ctx);
  if (error != LISP_ERROR_NONE) {
    fprintf(stderr, "%s: %s\n", expr, lisp_error_string(error));
    test_failures++;
    fclose(out);
    return;
  }
  lisp_printf(out, result);
  rewind(out);
  n = fread(printed, 1, sizeof(printed) - 1, out);
  printed[n] = '\0';
  fclose(out);
  testCheckStr(__FILE__, __LINE__, expr, printed, expected);
}

static void checkError(const char *expr) {
  LispError error = LISP_ERROR_NONE;
  lisp_eval(lisp_read(expr, &error, ctx), &error, ctx);
  if (error == LISP_ERROR_NONE) {
    fprintf(stderr, "%s: expected an error\n", expr);
    test_failures++;
  }
}

static void testMap(void) {
  checkEval("(map (lambda (x) (* x x)) '(1 2 3))", "(1 4 9)");
  checkEval("(map + '(1 2 3) '(10 20 30))", "(11 22 33)");
  checkEval("(map + '(1 2 3) '(10 20))", "(11 22)");
  checkEval("(map car '())", "NIL");
  checkEval("(map1 car '((a 1) (b 2)))", "(a b)");
  checkEval("(let ((acc '())) (for-each (lambda (x) (set! acc (cons x acc))) "
            "'(1 2 3)) acc)",
            "(3 2 1)");
  checkEval("(let ((acc '())) (for-each (lambda (x y) (set! acc (cons (+ x y) "
            "acc))) '(1 2 3) '(4 5 6)) acc)",
            "(9 7 5)");
  checkEval("(filter even? '(1 2 3 4 5 6))", "(2 4 6)");
  checkEval("(filter (lambda (e) e) '(1 #f 2 #f))", "(1 2)");
  checkEval("(reduce + 0 '(1 2 3 4))", "10");
  checkEval("(reduce + 0 '())", "0");
  checkEval("(fold-left cons '() '(1 2 3))", "(((NIL . 1) . 2) . 3)");
  checkEval("(apply (lambda (a . r) (list a r)) '(1 2 3))", "(1 (2 3))");
  // An escape from inside the callback, and an error raised there.
  checkEval("(call/cc (lambda (k) (map (lambda (x) (if (= x 2) (k 'out) x)) "
            "'(1 2 3))))",
            "out");
  checkError("(filter (lambda (x) (error \"boom\" x)) '(1))");
  // A collection inside the callback moves everything map holds.
  checkEval("(map (lambda (x y) (gc-flip) (cons x y)) '(1 2) '(a b))",
            "((1 . a) (2 . b))");
}

static void testSearch(void) {
  checkEval("(assoc \"b\" '((\"a\" . 1) (\"b\" . 2)))", "(\"b\" . 2)");
  checkEval("(assoc \"z\" '((\"a\" . 1)))", "#f");
  checkEval("(assq 'b '(x (a 1) (b 2)))", "(b 2)");
  checkEval("(assv 2 '((1 . a) (2 . b)))", "(2 . b)");
  checkEval("(member \"b\" '(\"a\" \"b\" \"c\"))", "(\"b\" \"c\")");
  checkEval("(memq 'c '(a b c d))", "(c d)");
  checkEval("(memv 9 '(1 2))", "#f");
}

static void testLists(void) {
  checkEval("(let ((l (list 1 2 3))) (list (reverse l) l))",
            "((3 2 1) (1 2 3))");
  checkEval("(last-pair '(1 2 . 3))", "(2 . 3)");
  checkEval("(list-tail '(1 2 3 4) 2)", "(3 4)");
  checkEval("(append)", "NIL");
  checkEval("(append '(1 2) '(3) '() '(4 5))", "(1 2 3 4 5)");
  checkEval("(append '(1) 2)", "(1 . 2)");
  checkEval("(let ((a (list 1 2))) (append a '(3)) a)", "(1 2)");
}

static void testSort(void) {
  checkEval("(sort '(3 1 2 5 4) <)", "(1 2 3 4 5)");
  checkEval("(sort '() <)", "NIL");
  checkEval("(sort (list \"pear\" \"apple\" \"fig\") string<?)",
            "(\"apple\" \"fig\" \"pear\")");
  // Stable: equal keys keep their order.
  checkEval("(sort '((a . 1) (b . 0) (c . 1) (d . 0)) "
            "(lambda (x y) (< (cdr x) (cdr y))))",
            "((b . 0) (d . 0) (a . 1) (c . 1))");
  checkEval("(let ((v (vector 5 3 9 1 7))) (sort! v <) v)", "#(1 3 5 7 9)");
  checkEval("(let ((v (vector 3 1 2))) (list (sort v <) v))",
            "(#(1 2 3) #(3 1 2))");
}

static void testFrames(void) {
  // Frames are sized for their parameters; defines inside still fit.
  checkEval("((lambda () (define x 10) (define y 20) (+ x y)))", "30");
  checkEval("((lambda (a b c d e f) (define g 7) (+ a b c d e f g)) "
            "1 2 3 4 5 6)",
            "28");
  checkEval("((lambda (a . r) (define z 1) (list a r z)) 1 2 3)",
            "(1 (2 3) 1)");
  checkEval("(let ((h (make-hash-table))) (do ((i 0 (+ i 1))) ((= i 100)) "
            "(hash-table-set! h i (* i i))) (list (hash-table-size h) "
            "(hash-table-ref h 99 (lambda () 'none))))",
            "(100 9801)");
}

int main(void) {
  testInit();
  ctx = lisp_init();
  // Keep the interpreter's messages for the error cases out of the output.
  lisp_set_stderr(tmpfile(), ctx);
  lisp_lib_load(ctx);
  testMap();
  testSearch();
  testLists();
  testSort();
  testFrames();
  lisp_shutdown(ctx);
  return testDone();
}
