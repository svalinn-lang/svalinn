CC ?= gcc
EXAMPLES = hello features unsafe_ffi sieve args8 structret classes generics interfaces destructors floats async wordcount

svalinnc: src/svalinnc.c
	$(CC) -O2 -o $@ src/svalinnc.c

selfhost/lexer: svalinnc selfhost/lexer.svl selfhost/lex.svl lib/std/io.svl lib/std/mem.svl
	./svalinnc selfhost/lexer.svl -o $@

selfhost/parser: svalinnc selfhost/parser.svl selfhost/lex.svl selfhost/parse.svl selfhost/dump.svl selfhost/types.svl selfhost/ast.svl selfhost/err.svl lib/std/io.svl lib/std/mem.svl
	./svalinnc selfhost/parser.svl -o $@

selfhost/semac: svalinnc selfhost/semac.svl selfhost/sema.svl selfhost/parse.svl selfhost/dump.svl selfhost/types.svl selfhost/ast.svl selfhost/err.svl selfhost/lex.svl lib/std/io.svl lib/std/mem.svl
	./svalinnc selfhost/semac.svl -o $@

SELF = selfhost/svalinnc.svl selfhost/gen.svl selfhost/opt.svl selfhost/sema.svl selfhost/parse.svl selfhost/dump.svl selfhost/types.svl selfhost/ast.svl selfhost/err.svl selfhost/lex.svl lib/std/io.svl lib/std/mem.svl lib/std/sys.svl

# stage 1 = svalinnc.svl compiled by the C compiler; stage 2 = svalinnc.svl compiled by stage 1
selfhost/svalinnc1: svalinnc $(SELF)
	./svalinnc selfhost/svalinnc.svl -o $@
selfhost/svalinnc2: selfhost/svalinnc1
	./selfhost/svalinnc1 selfhost/svalinnc.svl -o $@
# optimized bootstrap: svalinnc.svl compiled with -O by the C compiler, then by itself
selfhost/svalinnc1o: svalinnc $(SELF)
	./svalinnc -O selfhost/svalinnc.svl -o $@
selfhost/svalinnc2o: selfhost/svalinnc1o
	./selfhost/svalinnc1o -O selfhost/svalinnc.svl -o $@

test: svalinnc selfhost/lexer selfhost/parser selfhost/semac selfhost/svalinnc2 selfhost/svalinnc2o
	@for f in $(EXAMPLES); do ./svalinnc examples/$$f.svl -o examples/$$f && ./examples/$$f > /dev/null && echo "ok $$f"; done
	@./svalinnc examples/tokens.svl -o examples/tokens && ./examples/tokens examples/hello.svl > /dev/null && echo "ok tokens"
	@bad=0; for t in tests/*.svl; do [ $$t = tests/oob.svl ] && continue; ./svalinnc $$t -o /tmp/neg.bin > /tmp/neg.err 2>&1 && { echo "UNEXPECTED PASS $$t"; bad=1; }; \
	  cmp -s /tmp/neg.err $${t%.svl}.err || { echo "ERROR MESSAGE CHANGED $$t"; diff $${t%.svl}.err /tmp/neg.err | head -5; bad=1; }; done; \
	  [ $$bad = 0 ] && echo "ok negative tests (expected error messages)"
	@for t in tests/pass/*.svl; do ./svalinnc $$t -o /tmp/pass.bin > /dev/null && echo "ok compiles $$t"; done
	@./tests/security/run.sh | tail -1
	@./tests/run/run.sh ./svalinnc
	@bad=0; for f in examples/*.svl tests/*.svl selfhost/*.svl lib/std/*.svl selfhost/edge.txt; do \
	  ./svalinnc --dump-tokens $$f > /tmp/c.tok 2>&1; ./selfhost/lexer $$f > /tmp/ag.tok 2>&1; \
	  cmp -s /tmp/c.tok /tmp/ag.tok || { echo "LEXER MISMATCH $$f"; bad=1; }; done; \
	  [ $$bad = 0 ] && echo "ok self-hosted lexer matches C lexer on all files"
	@bad=0; for f in examples/*.svl tests/*.svl tests/pass/*.svl tests/run/*.svl tests/io/*.svl tests/testmode/*.svl selfhost/*.svl lib/std/*.svl selfhost/edge_parse.txt; do \
	  ./svalinnc --dump-ast $$f > /tmp/c.ast 2>&1; ./selfhost/parser $$f > /tmp/ag.ast 2>&1; \
	  cmp -s /tmp/c.ast /tmp/ag.ast || { echo "PARSER MISMATCH $$f"; bad=1; }; done; \
	  [ $$bad = 0 ] && echo "ok self-hosted parser matches C parser on all files"
	@bad=0; for f in examples/*.svl tests/*.svl tests/pass/*.svl tests/run/*.svl tests/io/*.svl tests/testmode/*.svl selfhost/*.svl lib/std/*.svl selfhost/edge_parse.txt; do \
	  ./svalinnc --dump-sema $$f > /tmp/c.sem 2>&1; ./selfhost/semac $$f > /tmp/ag.sem 2>&1; \
	  cmp -s /tmp/c.sem /tmp/ag.sem || { echo "SEMA MISMATCH $$f"; bad=1; }; done; \
	  [ $$bad = 0 ] && echo "ok self-hosted sema (types, capabilities, errors) matches C on all files"
	@bad=0; for f in examples/*.svl tests/pass/*.svl tests/run/*.svl tests/io/*.svl tests/testmode/*.svl selfhost/lexer.svl selfhost/parser.svl selfhost/semac.svl selfhost/svalinnc.svl; do \
	  ./svalinnc -S $$f -o /tmp/c.s > /dev/null 2>&1; ./selfhost/svalinnc2 -S $$f -o /tmp/a.s > /dev/null 2>&1; \
	  cmp -s /tmp/c.s /tmp/a.s || { echo "CODEGEN MISMATCH $$f"; bad=1; }; done; \
	  [ $$bad = 0 ] && echo "ok self-hosted code generator emits identical assembly"
	@./svalinnc -S selfhost/svalinnc.svl -o /tmp/b0.s; ./selfhost/svalinnc1 -S selfhost/svalinnc.svl -o /tmp/b1.s; ./selfhost/svalinnc2 -S selfhost/svalinnc.svl -o /tmp/b2.s; \
	  cmp -s /tmp/b0.s /tmp/b1.s && cmp -s /tmp/b1.s /tmp/b2.s && echo "ok BOOTSTRAP: C -> svalinnc1 -> svalinnc2 produce identical assembly (fixed point)"
	@./tests/run/run.sh ./selfhost/svalinnc2
	@./tests/run/run.sh "./svalinnc -O"
	@./tests/run/run.sh "./selfhost/svalinnc2o -O"
	@./tests/io/run.sh ./svalinnc && ./tests/io/run.sh ./selfhost/svalinnc2 && ./tests/io/run.sh "./svalinnc -O"
	@./tests/testmode/run.sh ./svalinnc && ./tests/testmode/run.sh ./selfhost/svalinnc2
	@bad=0; for f in examples/*.svl tests/pass/*.svl tests/run/*.svl bench/*.svl selfhost/lexer.svl selfhost/parser.svl selfhost/semac.svl selfhost/svalinnc.svl; do \
	  ./svalinnc -O -S $$f -o /tmp/c.s > /dev/null 2>&1; ./selfhost/svalinnc2 -O -S $$f -o /tmp/a.s > /dev/null 2>&1; \
	  cmp -s /tmp/c.s /tmp/a.s || { echo "OPTIMIZER MISMATCH $$f"; bad=1; }; done; \
	  [ $$bad = 0 ] && echo "ok self-hosted optimizer emits identical assembly (-O)"
	@./svalinnc -O -S selfhost/svalinnc.svl -o /tmp/o0.s; ./selfhost/svalinnc1o -O -S selfhost/svalinnc.svl -o /tmp/o1.s; ./selfhost/svalinnc2o -O -S selfhost/svalinnc.svl -o /tmp/o2.s; \
	  cmp -s /tmp/o0.s /tmp/o1.s && cmp -s /tmp/o1.s /tmp/o2.s && echo "ok OPTIMIZED BOOTSTRAP: C -O -> svalinnc1o -> svalinnc2o produce identical assembly (fixed point)"
	@./tests/fuzz/run.sh 100 1 | tail -1
	@./tests/fuzz/run.sh 50 9000 ./selfhost/svalinnc2o | tail -1
	@./tests/fuzz/strings.sh 60 1 | tail -1
	@./tests/fuzz/strings.sh 20 5000 ./selfhost/svalinnc2o | tail -1

clean:
	rm -f svalinnc selfhost/lexer selfhost/parser selfhost/semac selfhost/svalinnc1 selfhost/svalinnc2 selfhost/svalinnc1o selfhost/svalinnc2o selfhost/ag1o selfhost/ag2o tests/*.bin $(addprefix examples/,$(EXAMPLES) tokens)
