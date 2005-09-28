(*
    Title:      Standard Basis Library: Vector and Array functor for polymorphic vectors and arrays
    Copyright   David C.J. Matthews 2005

	This library is free software; you can redistribute it and/or
	modify it under the terms of the GNU Lesser General Public
	License as published by the Free Software Foundation; either
	version 2.1 of the License, or (at your option) any later version.
	
	This library is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
	Lesser General Public License for more details.
	
	You should have received a copy of the GNU Lesser General Public
	License along with this library; if not, write to the Free Software
	Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*)

(* This is almost identical to the VectorOperations functor but works on polymorphic vectors and arrays.
   There may be a way to combine the two. *)

functor PolyVectorOperations(
	type 'a vector
	val length: 'a vector -> word
	val unsafeSub: 'a vector * word -> 'a
	val unsafeSet: 'a vector * word * 'a -> unit (* Array only *)
):
	sig
	val appi : ((int * 'a) -> unit) -> 'a vector -> unit
	val app : ('a -> unit) -> 'a vector -> unit
	val foldli : ((int * 'a * 'b) -> 'b) -> 'b -> 'a vector -> 'b
	val foldri : ((int * 'a * 'b) -> 'b) -> 'b -> 'a vector -> 'b
	val foldl : (('a * 'b) -> 'b) -> 'b -> 'a vector -> 'b
	val foldr : (('a * 'b) -> 'b) -> 'b -> 'a vector -> 'b
	val modifyi : ((int * 'a) -> 'a) -> 'a vector -> unit (* Array only *)
	val modify : ('a -> 'a) -> 'a vector -> unit (* Array only *)
	val findi: (int * 'a -> bool) -> 'a vector -> (int * 'a) option
	val find: ('a -> bool) -> 'a vector -> 'a option
	val exists: ('a -> bool) -> 'a vector -> bool
	val all: ('a -> bool) -> 'a vector -> bool
	val collate: ('a * 'a -> order) -> 'a vector * 'a vector -> order
	end =
struct
		val wordAsInt: word -> int = RunCall.unsafeCast
		
		(* Apply a function to each element in turn *)
		fun appi f vec =
		let
			val len = length vec
			fun doapp j =
				if j >= len then ()
				else (f(wordAsInt j, unsafeSub(vec, j)); doapp(j+0w1))
		in
			doapp 0w0
		end
	
		fun app f vec =
		let		
			val len = length vec
			fun doapp j = 
				if j >= len then ()
				else (f(unsafeSub(vec, j)); doapp(j+0w1))
		in
			doapp 0w0
		end
		
		(* Fold a function over a array. *)
		(* foldl - increasing index *)
		fun foldl f init vec =
		let
			val len = length vec
			fun dofold j acc = 
				if j >= len then acc
				else dofold (j+0w1) (f (unsafeSub(vec, j), acc))
		in
			dofold 0w0 init
		end
	
		fun foldli f init vec =
		let	
			val len = length vec
			fun dofold j acc = 
				if j >= len then acc
				else dofold (j+0w1) (f (wordAsInt j, unsafeSub(vec, j), acc))
		in
			dofold 0w0 init
		end
	
		(* foldr - decreasing index *)
		fun foldr f init vec =
		let
			val len = length vec
			fun dofold j acc = 
				if j = 0w0 then acc
				else dofold (j-0w1) (f (unsafeSub(vec, j-0w1), acc))
		in
			dofold len init
		end
		
		fun foldri f init vec =
		let
			val len = length vec
			fun dofold j acc = 
				if j = 0w0 then acc
				else dofold (j-0w1) (f (wordAsInt(j-0w1), unsafeSub(vec, j-0w1), acc))
		in
			dofold len init
		end
		
		(* Apply a function to each element in turn and update the array with the
		   new values. *)
		fun modifyi f vec =
		let						
			val len = length vec
			fun doupdate j =
				if j >= len then ()
				else (unsafeSet(vec, j, f(wordAsInt j, unsafeSub(vec, j)));
					  doupdate(j+0w1))
		in
			doupdate 0w0
		end
	
		fun modify f vec =
		let
			val len = length vec
			fun doupdate j = 
				if j >= len then ()
				else (unsafeSet(vec, j, f(unsafeSub(vec, j))); doupdate(j+0w1))
		in
			doupdate 0w0
		end

		(* Find a character that matches the predicate. *)
		fun findi pred vec =
		let	
			val len = length vec
			fun dofind j = 
				if j >= len then NONE
				else
				let
					val v = unsafeSub(vec, j)
				in
					if pred(wordAsInt j, v)
					then SOME (wordAsInt j, v)
					else dofind (j+0w1)
				end
		in
			dofind 0w0
		end
		
		fun find pred vec =
		let
			val len = length vec
			fun dofind j = 
				if j >= len then NONE
				else
				let
					val v = unsafeSub(vec, j)
				in
					if pred v
					then SOME v
					else dofind (j+0w1)
				end
		in
			dofind 0w0
		end

		fun exists f arr = Option.isSome(find f arr)
		
		fun all pred arr = not (exists (not o pred) arr)

		fun collate cmp (vec1, vec2) =
		let	
			val len1 = length vec1 and len2 = length vec2
			(* Keep comparing items until either we come to the end of one of the arrays or
			   we find a mismatch. *)
			fun dotest j =
				if j >= len1 then if len1 = len2 then EQUAL else (* j < len2 *) LESS
				else if j >= len2 then (* But j < len1, so a1 is longer *) GREATER
				else case cmp(unsafeSub(vec1, j), unsafeSub(vec2, j)) of
					LESS => LESS
				|	GREATER => GREATER
				|	EQUAL => dotest (j+0w1)
		in
			dotest 0w0
		end
end;
