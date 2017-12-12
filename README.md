Negation in DaaT Dynamic Pruning
======================


Reference
---------
If you use this code for your own experiments, please cite the following work:

Joel Mackenzie, Craig Macdonald, Falk Scholer and J. Shane Culpepper: **On the Cost of Negation for Dynamic Pruning.** In Proceedings of the 40th European Conference on Information Retrieval, ECIR, 2018. 


Acknowledgements
----------------
This code is derived from the [Quant-BM-Wand](http://github.com/JMMackenzie/Quant-BM-Wand/) codebase, which,
in turn, was derived from many others such as:
- [ATIRE](https://atire.org/)
- [WandBL](https://github.com/jsc/WANDbl)
- [JASS](https://github.com/lintool/JASS)
- [FastPFor](https://github.com/lemire/FastPFor)
- [FastDifferentialCoding](https://github.com/lemire/FastDifferentialCoding)
- [SDSL](https://github.com/simongog/sdsl-lite)

Please see each of these repo's for further information.

Licence
-------
The licence is provided, see the LICENCE file.

Usage
=====
Usage is similar to the original Quant-BM-Wand codebase.
For our experiments, we utilised frequency indexes, but the code will still work with quantized postings.

Query Format
------------
Since we included the negation operator, we modified the parser to identify terms with the
negation operator `-`. Queries follow the same format as before, for example:
```
123;test query -negated
```
The excite query file we used for the experiments is also provided in the `ir-repo/` directory in both
the `.negated` and `.disjunctive` formats. Note that these have been s-stemmed.
