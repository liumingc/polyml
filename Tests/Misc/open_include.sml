val _ = PolyML.Compiler.parsetree := true;
val _ = PolyML.Compiler.icode := true;

structure Foo =
struct
	fun foo () = print "this is foo\n"
end

structure Bar =
struct
	open Foo
	fun bar () =
	(
		foo();
		print "+ this is bar\n"
	)
end

val _ =
	(
		Bar.bar();
		print "more coming\n";
		Bar.foo()
	)
