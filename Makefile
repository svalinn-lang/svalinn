CC ?= gcc
EXAMPLES = hello features unsafe_ffi sieve args8 structret classes generics interfaces destructors floats async wordcount

aegisc: src/aegisc.c
	$(CC) -O2 -o $@ src/aegisc.c

selfhost/lexer: aegisc selfhost/lexer.ag selfhost/lex.ag lib/std/io.ag lib/std/mem.ag
	./aegisc selfhost/lexer.ag -o $@

selfhost/parser: aegisc selfhost/parser.ag selfhost/lex.ag selfhost/parse.ag selfhost/dump.ag selfhost/types.ag selfhost/ast.ag selfhost/err.ag lib/std/io.ag lib/std/mem.ag
	./aegisc selfhost/parser.ag -o $@

selfhost/semac: aegisc selfhost/semac.ag selfhost/sema.ag selfhost/parse.ag selfhost/dump.ag selfhost/types.ag selfhost/ast.ag selfhost/err.ag selfhost/lex.ag lib/std/io.ag lib/std/mem.ag
	./aegisc selfhost/semac.ag -o $@

SELF = selfhost/aegisc.ag selfhost/gen.ag selfhost/opt.ag selfhost/sema.ag selfhost/parse.ag selfhost/dump.ag selfhost/types.ag selfhost/ast.ag selfhost/err.ag selfhost/lex.ag lib/std/io.ag lib/std/mem.ag lib/std/sys.ag

# stage 1 = aegisc.ag compiled by the C compiler; stage 2 = aegisc.ag compiled by stage 1
selfhost/aegisc1: aegisc $(SELF)
	./aegisc selfhost/aegisc.ag -o $@
selfhost/aegisc2: selfhost/aegisc1
	./selfhost/aegisc1 selfhost/aegisc.ag -o $@
# optimized bootstrap: aegisc.ag compiled with -O by the C compiler, then by itself
selfhost/aegisc1o: aegisc $(SELF)
	./aegisc -O selfhost/aegisc.ag -o $@
selfhost/aegisc2o: selfhost/aegisc1o
	./selfhost/aegisc1o -O selfhost/aegisc.ag -o $@

test: aegisc selfhost/lexer selfhost/parser selfhost/semac selfhost/aegisc2 selfhost/aegisc2o
	@for f in $(EXAMPLES); do ./aegisc examples/$$f.ag -o examples/$$f && ./examples/$$f > /dev/null && echo "ok $$f"; done
	@./aegisc examples/tokens.ag -o examples/tokens && ./examples/tokens examples/hello.ag > /dev/null && echo "ok tokens"
	@bad=0; for t in tests/*.ag; do [ $$t = tests/oob.ag ] && continue; ./aegisc $$t -o /tmp/neg.bin > /tmp/neg.err 2>&1 && { echo "UNEXPECTED PASS $$t"; bad=1; }; \
	  cmp -s /tmp/neg.err $${t%.ag}.err || { echo "ERROR MESSAGE CHANGED $$t"; diff $${t%.ag}.err /tmp/neg.err | head -5; bad=1; }; done; \
	  [ $$bad = 0 ] && echo "ok negative tests (expected error messages)"
	@for t in tests/pass/*.ag; do ./aegisc $$t -o /tmp/pass.bin > /dev/null && echo "ok compiles $$t"; done
	@./tests/security/run.sh | tail -1
	@./tests/run/run.sh ./aegisc
	@bad=0; for f in examples/*.ag tests/*.ag selfhost/*.ag lib/std/*.ag selfhost/edge.txt; do \
	  ./aegisc --dump-tokens $$f > /tmp/c.tok 2>&1; ./selfhost/lexer $$f > /tmp/ag.tok 2>&1; \
	  cmp -s /tmp/c.tok /tmp/ag.tok || { echo "LEXER MISMATCH $$f"; bad=1; }; done; \
	  [ $$bad = 0 ] && echo "ok self-hosted lexer matches C lexer on all files"
	@bad=0; for f in examples/*.ag tests/*.ag tests/pass/*.ag tests/run/*.ag tests/io/*.ag tests/testmode/*.ag selfhost/*.ag lib/std/*.ag selfhost/edge_parse.txt; do \
	  ./aegisc --dump-ast $$f > /tmp/c.ast 2>&1; ./selfhost/parser $$f > /tmp/ag.ast 2>&1; \
	  cmp -s /tmp/c.ast /tmp/ag.ast || { echo "PARSER MISMATCH $$f"; bad=1; }; done; \
	  [ $$bad = 0 ] && echo "ok self-hosted parser matches C parser on all files"
	@bad=0; for f in examples/*.ag tests/*.ag tests/pass/*.ag tests/run/*.ag tests/io/*.ag tests/testmode/*.ag selfhost/*.ag lib/std/*.ag selfhost/edge_parse.txt; do \
	  ./aegisc --dump-sema $$f > /tmp/c.sem 2>&1; ./selfhost/semac $$f > /tmp/ag.sem 2>&1; \
	  cmp -s /tmp/c.sem /tmp/ag.sem || { echo "SEMA MISMATCH $$f"; bad=1; }; done; \
	  [ $$bad = 0 ] && echo "ok self-hosted sema (types, capabilities, errors) matches C on all files"
	@bad=0; for f in examples/*.ag tests/pass/*.ag tests/run/*.ag tests/io/*.ag tests/testmode/*.ag selfhost/lexer.ag selfhost/parser.ag selfhost/semac.ag selfhost/aegisc.ag; do \
	  ./aegisc -S $$f -o /tmp/c.s > /dev/null 2>&1; ./selfhost/aegisc2 -S $$f -o /tmp/a.s > /dev/null 2>&1; \
	  cmp -s /tmp/c.s /tmp/a.s || { echo "CODEGEN MISMATCH $$f"; bad=1; }; done; \
	  [ $$bad = 0 ] && echo "ok self-hosted code generator emits identical assembly"
	@./aegisc -S selfhost/aegisc.ag -o /tmp/b0.s; ./selfhost/aegisc1 -S selfhost/aegisc.ag -o /tmp/b1.s; ./selfhost/aegisc2 -S selfhost/aegisc.ag -o /tmp/b2.s; \
	  cmp -s /tmp/b0.s /tmp/b1.s && cmp -s /tmp/b1.s /tmp/b2.s && echo "ok BOOTSTRAP: C -> aegisc1 -> aegisc2 produce identical assembly (fixed point)"
	@./tests/run/run.sh ./selfhost/aegisc2
	@./tests/run/run.sh "./aegisc -O"
	@./tests/run/run.sh "./selfhost/aegisc2o -O"
	@./tests/io/run.sh ./aegisc && ./tests/io/run.sh ./selfhost/aegisc2 && ./tests/io/run.sh "./aegisc -O"
	@./tests/testmode/run.sh ./aegisc && ./tests/testmode/run.sh ./selfhost/aegisc2
	@bad=0; for f in examples/*.ag tests/pass/*.ag tests/run/*.ag bench/*.ag selfhost/lexer.ag selfhost/parser.ag selfhost/semac.ag selfhost/aegisc.ag; do \
	  ./aegisc -O -S $$f -o /tmp/c.s > /dev/null 2>&1; ./selfhost/aegisc2 -O -S $$f -o /tmp/a.s > /dev/null 2>&1; \
	  cmp -s /tmp/c.s /tmp/a.s || { echo "OPTIMIZER MISMATCH $$f"; bad=1; }; done; \
	  [ $$bad = 0 ] && echo "ok self-hosted optimizer emits identical assembly (-O)"
	@./aegisc -O -S selfhost/aegisc.ag -o /tmp/o0.s; ./selfhost/aegisc1o -O -S selfhost/aegisc.ag -o /tmp/o1.s; ./selfhost/aegisc2o -O -S selfhost/aegisc.ag -o /tmp/o2.s; \
	  cmp -s /tmp/o0.s /tmp/o1.s && cmp -s /tmp/o1.s /tmp/o2.s && echo "ok OPTIMIZED BOOTSTRAP: C -O -> aegisc1o -> aegisc2o produce identical assembly (fixed point)"
	@./tests/fuzz/run.sh 100 1 | tail -1
	@./tests/fuzz/run.sh 50 9000 ./selfhost/aegisc2o | tail -1
	@./tests/fuzz/strings.sh 60 1 | tail -1
	@./tests/fuzz/strings.sh 20 5000 ./selfhost/aegisc2o | tail -1

clean:
	rm -f aegisc selfhost/lexer selfhost/parser selfhost/semac selfhost/aegisc1 selfhost/aegisc2 selfhost/aegisc1o selfhost/aegisc2o selfhost/ag1o selfhost/ag2o tests/*.bin $(addprefix examples/,$(EXAMPLES) tokens)
