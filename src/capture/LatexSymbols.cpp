#include "LatexSymbols.h"

#include <QHash>

#include <array>

namespace ws::capture::latex {

namespace {

using C = Class;

// Typst names only where they have stayed the same for years; the rest are
// written as their Unicode characters, which Typst's math takes as they are.
constexpr std::array kSymbols = std::to_array<Symbol>({
    // Greek
    {u"alpha", u"α", C::Ord, u"alpha"}, {u"beta", u"β", C::Ord, u"beta"}, {u"gamma", u"γ", C::Ord, u"gamma"},
    {u"delta", u"δ", C::Ord, u"delta"}, {u"epsilon", u"ϵ", C::Ord, u"epsilon.alt"},
    {u"varepsilon", u"ε", C::Ord, u"epsilon"}, {u"zeta", u"ζ", C::Ord, u"zeta"}, {u"eta", u"η", C::Ord, u"eta"},
    {u"theta", u"θ", C::Ord, u"theta"}, {u"vartheta", u"ϑ", C::Ord, u"theta.alt"}, {u"iota", u"ι", C::Ord, u"iota"},
    {u"kappa", u"κ", C::Ord, u"kappa"}, {u"varkappa", u"ϰ", C::Ord, u"kappa.alt"},
    {u"lambda", u"λ", C::Ord, u"lambda"}, {u"mu", u"μ", C::Ord, u"mu"}, {u"nu", u"ν", C::Ord, u"nu"},
    {u"xi", u"ξ", C::Ord, u"xi"}, {u"omicron", u"ο", C::Ord, u"omicron"}, {u"pi", u"π", C::Ord, u"pi"},
    {u"varpi", u"ϖ", C::Ord, u"pi.alt"}, {u"rho", u"ρ", C::Ord, u"rho"}, {u"varrho", u"ϱ", C::Ord, u"rho.alt"},
    {u"sigma", u"σ", C::Ord, u"sigma"}, {u"varsigma", u"ς", C::Ord, u"sigma.alt"}, {u"tau", u"τ", C::Ord, u"tau"},
    {u"upsilon", u"υ", C::Ord, u"upsilon"}, {u"phi", u"ϕ", C::Ord, u"phi.alt"}, {u"varphi", u"φ", C::Ord, u"phi"},
    {u"chi", u"χ", C::Ord, u"chi"}, {u"psi", u"ψ", C::Ord, u"psi"}, {u"omega", u"ω", C::Ord, u"omega"},
    {u"Gamma", u"Γ", C::Ord, u"Gamma"}, {u"Delta", u"Δ", C::Ord, u"Delta"}, {u"Theta", u"Θ", C::Ord, u"Theta"},
    {u"Lambda", u"Λ", C::Ord, u"Lambda"}, {u"Xi", u"Ξ", C::Ord, u"Xi"}, {u"Pi", u"Π", C::Ord, u"Pi"},
    {u"Sigma", u"Σ", C::Ord, u"Sigma"}, {u"Upsilon", u"Υ", C::Ord, u"Upsilon"}, {u"Phi", u"Φ", C::Ord, u"Phi"},
    {u"Psi", u"Ψ", C::Ord, u"Psi"}, {u"Omega", u"Ω", C::Ord, u"Omega"},
    {u"varGamma", u"Γ", C::Ord, u"Gamma"}, {u"varDelta", u"Δ", C::Ord, u"Delta"}, {u"varTheta", u"Θ", C::Ord, u"Theta"},
    {u"varLambda", u"Λ", C::Ord, u"Lambda"}, {u"varXi", u"Ξ", C::Ord, u"Xi"}, {u"varPi", u"Π", C::Ord, u"Pi"},
    {u"varSigma", u"Σ", C::Ord, u"Sigma"}, {u"varUpsilon", u"Υ", C::Ord, u"Upsilon"}, {u"varPhi", u"Φ", C::Ord, u"Phi"},
    {u"varPsi", u"Ψ", C::Ord, u"Psi"}, {u"varOmega", u"Ω", C::Ord, u"Omega"},
    // Other letters and signs
    {u"infty", u"∞", C::Ord, u"infinity"}, {u"partial", u"∂", C::Ord, {}}, {u"nabla", u"∇", C::Ord, u"nabla"},
    {u"hbar", u"ℏ", C::Ord, {}}, {u"hslash", u"ℏ", C::Ord, {}}, {u"ell", u"ℓ", C::Ord, u"ell"},
    {u"Re", u"ℜ", C::Ord, {}}, {u"Im", u"ℑ", C::Ord, {}}, {u"wp", u"℘", C::Ord, {}}, {u"aleph", u"ℵ", C::Ord, u"aleph"},
    {u"beth", u"ℶ", C::Ord, {}}, {u"imath", u"ı", C::Ord, {}}, {u"jmath", u"ȷ", C::Ord, {}},
    {u"emptyset", u"∅", C::Ord, u"emptyset"}, {u"varnothing", u"∅", C::Ord, u"emptyset"},
    {u"forall", u"∀", C::Ord, u"forall"}, {u"exists", u"∃", C::Ord, u"exists"}, {u"nexists", u"∄", C::Ord, {}},
    {u"neg", u"¬", C::Ord, {}}, {u"lnot", u"¬", C::Ord, {}}, {u"angle", u"∠", C::Ord, u"angle"},
    {u"measuredangle", u"∡", C::Ord, {}}, {u"triangle", u"△", C::Ord, {}}, {u"square", u"□", C::Ord, {}},
    {u"Box", u"□", C::Ord, {}}, {u"blacksquare", u"■", C::Ord, {}}, {u"diamondsuit", u"♢", C::Ord, {}},
    {u"prime", u"′", C::Ord, u"prime"}, {u"backprime", u"‵", C::Ord, {}}, {u"degree", u"°", C::Ord, u"degree"},
    {u"circ", u"∘", C::Bin, {}}, {u"top", u"⊤", C::Ord, {}}, {u"bot", u"⊥", C::Ord, {}}, {u"surd", u"√", C::Ord, {}},
    {u"ldots", u"…", C::Ord, u"dots.h"}, {u"dots", u"…", C::Ord, u"dots.h"}, {u"dotsc", u"…", C::Ord, u"dots.h"},
    {u"cdots", u"⋯", C::Ord, {}}, {u"dotsb", u"⋯", C::Ord, {}}, {u"dotsm", u"⋯", C::Ord, {}},
    {u"vdots", u"⋮", C::Ord, u"dots.v"}, {u"ddots", u"⋱", C::Ord, u"dots.down"}, {u"therefore", u"∴", C::Rel, u"therefore"},
    {u"because", u"∵", C::Rel, u"because"}, {u"backslash", u"∖", C::Ord, {}}, {u"star", u"⋆", C::Bin, {}},
    {u"clubsuit", u"♣", C::Ord, {}}, {u"heartsuit", u"♡", C::Ord, {}}, {u"spadesuit", u"♠", C::Ord, {}},
    {u"flat", u"♭", C::Ord, {}}, {u"natural", u"♮", C::Ord, {}}, {u"sharp", u"♯", C::Ord, {}},
    {u"checkmark", u"✓", C::Ord, {}}, {u"dagger", u"†", C::Bin, {}}, {u"ddagger", u"‡", C::Bin, {}},
    {u"S", u"§", C::Ord, {}}, {u"P", u"¶", C::Ord, {}}, {u"copyright", u"©", C::Ord, {}},
    {u"%", u"%", C::Ord, {}}, {u"$", u"$", C::Ord, u"\\$"}, {u"#", u"#", C::Ord, u"\\#"}, {u"&", u"&", C::Ord, u"\\&"},
    {u"_", u"_", C::Ord, u"\\_"}, {u"{", u"{", C::Open, u"{"}, {u"}", u"}", C::Close, u"}"}, {u"|", u"‖", C::Ord, {}},
    // Large operators
    {u"sum", u"∑", C::Op, u"sum"}, {u"prod", u"∏", C::Op, u"product"}, {u"coprod", u"∐", C::Op, {}},
    {u"int", u"∫", C::Op, u"integral"}, {u"iint", u"∬", C::Op, u"integral.double"},
    {u"iiint", u"∭", C::Op, u"integral.triple"}, {u"oint", u"∮", C::Op, u"integral.cont"},
    {u"oiint", u"∯", C::Op, {}}, {u"intop", u"∫", C::Op, u"integral"}, {u"smallint", u"∫", C::Op, u"integral"},
    {u"bigcup", u"⋃", C::Op, {}}, {u"bigcap", u"⋂", C::Op, {}}, {u"bigsqcup", u"⨆", C::Op, {}},
    {u"bigvee", u"⋁", C::Op, {}}, {u"bigwedge", u"⋀", C::Op, {}}, {u"bigoplus", u"⨁", C::Op, {}},
    {u"bigotimes", u"⨂", C::Op, {}}, {u"bigodot", u"⨀", C::Op, {}}, {u"biguplus", u"⨄", C::Op, {}},
    // Binary operators
    {u"pm", u"±", C::Bin, u"plus.minus"},
    {u"mp", u"∓", C::Bin, u"minus.plus"}, {u"times", u"×", C::Bin, u"times"}, {u"div", u"÷", C::Bin, u"div"},
    {u"cdot", u"⋅", C::Bin, u"dot.op"}, {u"cdotp", u"⋅", C::Bin, u"dot.op"}, {u"centerdot", u"⋅", C::Bin, u"dot.op"},
    {u"ast", u"∗", C::Bin, {}}, {u"bullet", u"∙", C::Bin, {}}, {u"oplus", u"⊕", C::Bin, {}},
    {u"ominus", u"⊖", C::Bin, {}}, {u"otimes", u"⊗", C::Bin, {}}, {u"oslash", u"⊘", C::Bin, {}},
    {u"odot", u"⊙", C::Bin, {}}, {u"cup", u"∪", C::Bin, u"union"}, {u"cap", u"∩", C::Bin, {}},
    {u"sqcup", u"⊔", C::Bin, {}}, {u"sqcap", u"⊓", C::Bin, {}}, {u"vee", u"∨", C::Bin, {}}, {u"lor", u"∨", C::Bin, {}},
    {u"wedge", u"∧", C::Bin, {}}, {u"land", u"∧", C::Bin, {}}, {u"setminus", u"∖", C::Bin, {}},
    {u"smallsetminus", u"∖", C::Bin, {}}, {u"wr", u"≀", C::Bin, {}}, {u"diamond", u"⋄", C::Bin, {}},
    {u"uplus", u"⊎", C::Bin, {}}, {u"amalg", u"⨿", C::Bin, {}}, {u"triangleleft", u"◃", C::Bin, {}},
    {u"triangleright", u"▹", C::Bin, {}}, {u"bigtriangleup", u"△", C::Bin, {}},
    {u"bigtriangledown", u"▽", C::Bin, {}}, {u"ltimes", u"⋉", C::Bin, {}}, {u"rtimes", u"⋊", C::Bin, {}},
    {u"boxplus", u"⊞", C::Bin, {}}, {u"boxtimes", u"⊠", C::Bin, {}}, {u"divideontimes", u"⋇", C::Bin, {}},
    {u"bmod", u"mod", C::Bin, u"mod"},
    // Relations
    {u"leq", u"≤", C::Rel, u"<="}, {u"le", u"≤", C::Rel, u"<="}, {u"geq", u"≥", C::Rel, u">="},
    {u"ge", u"≥", C::Rel, u">="}, {u"leqslant", u"⩽", C::Rel, {}}, {u"geqslant", u"⩾", C::Rel, {}},
    {u"leqq", u"≦", C::Rel, {}}, {u"geqq", u"≧", C::Rel, {}}, {u"neq", u"≠", C::Rel, u"!="}, {u"ne", u"≠", C::Rel, u"!="},
    {u"lt", u"<", C::Rel, {}}, {u"gt", u">", C::Rel, {}}, {u"equiv", u"≡", C::Rel, u"equiv"},
    {u"approx", u"≈", C::Rel, u"approx"}, {u"approxeq", u"≊", C::Rel, {}}, {u"sim", u"∼", C::Rel, {}},
    {u"simeq", u"≃", C::Rel, {}}, {u"cong", u"≅", C::Rel, {}}, {u"ncong", u"≇", C::Rel, {}}, {u"nsim", u"≁", C::Rel, {}},
    {u"propto", u"∝", C::Rel, u"prop"}, {u"ll", u"≪", C::Rel, u"<<"}, {u"gg", u"≫", C::Rel, u">>"},
    {u"lll", u"⋘", C::Rel, {}}, {u"ggg", u"⋙", C::Rel, {}}, {u"lesssim", u"≲", C::Rel, {}}, {u"gtrsim", u"≳", C::Rel, {}},
    {u"nleq", u"≰", C::Rel, {}}, {u"ngeq", u"≱", C::Rel, {}}, {u"nless", u"≮", C::Rel, {}}, {u"ngtr", u"≯", C::Rel, {}},
    {u"prec", u"≺", C::Rel, {}}, {u"succ", u"≻", C::Rel, {}}, {u"preceq", u"⪯", C::Rel, {}}, {u"succeq", u"⪰", C::Rel, {}},
    {u"subset", u"⊂", C::Rel, u"subset"}, {u"supset", u"⊃", C::Rel, u"supset"},
    {u"subseteq", u"⊆", C::Rel, u"subset.eq"}, {u"supseteq", u"⊇", C::Rel, u"supset.eq"},
    {u"subsetneq", u"⊊", C::Rel, {}}, {u"supsetneq", u"⊋", C::Rel, {}}, {u"nsubseteq", u"⊈", C::Rel, {}},
    {u"nsupseteq", u"⊉", C::Rel, {}}, {u"sqsubseteq", u"⊑", C::Rel, {}}, {u"sqsupseteq", u"⊒", C::Rel, {}},
    {u"in", u"∈", C::Rel, u"in"}, {u"notin", u"∉", C::Rel, u"in.not"}, {u"ni", u"∋", C::Rel, {}},
    {u"owns", u"∋", C::Rel, {}}, {u"perp", u"⊥", C::Rel, u"perp"}, {u"parallel", u"∥", C::Rel, u"parallel"},
    {u"nparallel", u"∦", C::Rel, {}}, {u"mid", u"∣", C::Rel, {}}, {u"nmid", u"∤", C::Rel, {}},
    {u"vdash", u"⊢", C::Rel, {}}, {u"dashv", u"⊣", C::Rel, {}}, {u"models", u"⊨", C::Rel, {}},
    {u"vDash", u"⊨", C::Rel, {}}, {u"asymp", u"≍", C::Rel, {}}, {u"doteq", u"≐", C::Rel, {}},
    {u"triangleq", u"≜", C::Rel, {}}, {u"coloneqq", u"≔", C::Rel, {}}, {u"eqqcolon", u"≕", C::Rel, {}},
    {u"bowtie", u"⋈", C::Rel, {}}, {u"smile", u"⌣", C::Rel, {}}, {u"frown", u"⌢", C::Rel, {}},
    {u"to", u"→", C::Rel, u"->"}, {u"rightarrow", u"→", C::Rel, u"->"}, {u"leftarrow", u"←", C::Rel, u"<-"},
    {u"gets", u"←", C::Rel, u"<-"}, {u"leftrightarrow", u"↔", C::Rel, u"<->"}, {u"Rightarrow", u"⇒", C::Rel, u"=>"},
    {u"Leftarrow", u"⇐", C::Rel, {}}, {u"Leftrightarrow", u"⇔", C::Rel, u"<=>"}, {u"iff", u"⟺", C::Rel, u"<==>"},
    {u"implies", u"⟹", C::Rel, u"==>"}, {u"impliedby", u"⟸", C::Rel, {}}, {u"mapsto", u"↦", C::Rel, u"|->"},
    {u"longrightarrow", u"⟶", C::Rel, u"-->"}, {u"longleftarrow", u"⟵", C::Rel, u"<--"},
    {u"longleftrightarrow", u"⟷", C::Rel, {}}, {u"Longrightarrow", u"⟹", C::Rel, u"==>"},
    {u"Longleftarrow", u"⟸", C::Rel, {}}, {u"Longleftrightarrow", u"⟺", C::Rel, u"<==>"},
    {u"longmapsto", u"⟼", C::Rel, {}}, {u"uparrow", u"↑", C::Rel, {}}, {u"downarrow", u"↓", C::Rel, {}},
    {u"updownarrow", u"↕", C::Rel, {}}, {u"Uparrow", u"⇑", C::Rel, {}}, {u"Downarrow", u"⇓", C::Rel, {}},
    {u"nearrow", u"↗", C::Rel, {}}, {u"searrow", u"↘", C::Rel, {}}, {u"swarrow", u"↙", C::Rel, {}},
    {u"nwarrow", u"↖", C::Rel, {}}, {u"hookrightarrow", u"↪", C::Rel, {}}, {u"hookleftarrow", u"↩", C::Rel, {}},
    {u"rightleftharpoons", u"⇌", C::Rel, {}}, {u"leftrightharpoons", u"⇋", C::Rel, {}},
    {u"rightharpoonup", u"⇀", C::Rel, {}}, {u"leftharpoonup", u"↼", C::Rel, {}},
    {u"rightrightarrows", u"⇉", C::Rel, {}}, {u"leftleftarrows", u"⇇", C::Rel, {}},
    {u"twoheadrightarrow", u"↠", C::Rel, {}}, {u"rightsquigarrow", u"⇝", C::Rel, {}},
    // Delimiters
    {u"lbrace", u"{", C::Open, u"{"},
    {u"rbrace", u"}", C::Close, u"}"}, {u"lbrack", u"[", C::Open, {}}, {u"rbrack", u"]", C::Close, {}},
    {u"langle", u"⟨", C::Open, {}}, {u"rangle", u"⟩", C::Close, {}}, {u"lfloor", u"⌊", C::Open, {}},
    {u"rfloor", u"⌋", C::Close, {}}, {u"lceil", u"⌈", C::Open, {}}, {u"rceil", u"⌉", C::Close, {}},
    {u"lvert", u"|", C::Open, {}}, {u"rvert", u"|", C::Close, {}}, {u"lVert", u"‖", C::Open, {}},
    {u"rVert", u"‖", C::Close, {}}, {u"Vert", u"‖", C::Ord, {}}, {u"vert", u"|", C::Ord, {}},
    {u"lgroup", u"⟮", C::Open, {}}, {u"rgroup", u"⟯", C::Close, {}},
    {u"ulcorner", u"⌜", C::Open, {}}, {u"urcorner", u"⌝", C::Close, {}}, {u"colon", u":", C::Punct, {}},
});

// The characters typed as they are that are not ordinary (a letter or a
// digit is; so are the rest, / and . and | among them).
constexpr std::array kCharacters = std::to_array<Symbol>({
    {u"+", u"+", C::Bin, {}}, {u"-", u"−", C::Bin, {}}, {u"*", u"∗", C::Bin, {}},
    {u"=", u"=", C::Rel, {}}, {u"<", u"<", C::Rel, {}}, {u">", u">", C::Rel, {}}, {u":", u":", C::Rel, {}},
    {u"(", u"(", C::Open, {}}, {u"[", u"[", C::Open, {}}, {u")", u")", C::Close, {}}, {u"]", u"]", C::Close, {}},
    {u",", u",", C::Punct, {}}, {u";", u";", C::Punct, {}}, {u"!", u"!", C::Close, {}}, {u"?", u"?", C::Close, {}},
    {u"'", u"′", C::Ord, u"'"},
});

constexpr std::array kFunctions = std::to_array<QStringView>({u"sin", u"cos", u"tan", u"cot", u"sec", u"csc",
    u"arcsin", u"arccos", u"arctan", u"sinh", u"cosh", u"tanh", u"coth", u"log", u"ln", u"lg", u"exp", u"arg", u"deg",
    u"dim", u"ker", u"hom"});
constexpr std::array kLimitFunctions = std::to_array<QStringView>({u"lim", u"liminf", u"limsup", u"max", u"min",
    u"sup", u"inf", u"det", u"gcd", u"Pr"});

constexpr std::array kAccents = std::to_array<Accent>({
    {u"hat", u"^", false, false, u"hat"}, {u"widehat", u"^", false, true, u"hat"},
    {u"check", u"ˇ", false, false, u"caron"}, {u"widecheck", u"ˇ", false, true, u"caron"},
    {u"tilde", u"~", false, false, u"tilde"}, {u"widetilde", u"~", false, true, u"tilde"},
    {u"acute", u"´", false, false, u"acute"}, {u"grave", u"`", false, false, u"grave"},
    {u"dot", u"˙", false, false, u"dot"}, {u"ddot", u"¨", false, false, u"dot.double"},
    {u"dddot", u"⃛", false, false, u"dot.triple"}, {u"breve", u"˘", false, false, u"breve"},
    {u"bar", u"¯", false, false, u"macron"}, {u"vec", u"→", false, false, u"arrow"},
    {u"mathring", u"˚", false, false, u"circle"}, {u"overline", u"¯", false, true, u"overline"},
    {u"underline", u"_", true, true, u"underline"}, {u"overrightarrow", u"→", false, true, u"arrow"},
    {u"overleftarrow", u"←", false, true, u"arrow.l"}, {u"overleftrightarrow", u"↔", false, true, u"arrow.l.r"},
    {u"underrightarrow", u"→", true, true, {}}, {u"underleftarrow", u"←", true, true, {}},
    {u"overbrace", u"⏞", false, true, u"overbrace"}, {u"underbrace", u"⏟", true, true, u"underbrace"},
    {u"overbracket", u"⎴", false, true, u"overbracket"}, {u"underbracket", u"⎵", true, true, u"underbracket"},
});

constexpr std::array kFonts = std::to_array<Font>({
    {u"mathbf", u"bold", u"bold"}, {u"boldsymbol", u"bold-italic", u"bold"}, {u"bm", u"bold-italic", u"bold"},
    {u"pmb", u"bold-italic", u"bold"}, {u"mathrm", u"normal", u"upright"}, {u"mathit", u"italic", u"italic"},
    {u"mathsf", u"sans-serif", u"sans"}, {u"mathtt", u"monospace", u"mono"}, {u"mathcal", u"script", u"cal"},
    {u"mathscr", u"script", u"cal"}, {u"mathfrak", u"fraktur", u"frak"}, {u"mathbb", u"double-struck", u"bb"},
    {u"mathnormal", u"italic", u"italic"}, {u"Bbb", u"double-struck", u"bb"},
});

struct Space {
    QStringView name;
    double em;
};
constexpr std::array kSpaces = std::to_array<Space>({{u",", 3.0 / 18}, {u":", 4.0 / 18}, {u">", 4.0 / 18},
    {u";", 5.0 / 18}, {u" ", 1.0 / 3}, {u"~", 1.0 / 3}, {u"quad", 1}, {u"qquad", 2}, {u"enspace", 0.5},
    {u"enskip", 0.5}, {u"thinspace", 3.0 / 18}, {u"medspace", 4.0 / 18}, {u"thickspace", 5.0 / 18}, {u"!", 0},
    {u"negthinspace", 0}, {u"negmedspace", 0}, {u"negthickspace", 0}, {u"space", 1.0 / 3}, {u"nobreakspace", 1.0 / 3}});

// "\\alpha" -> "alpha"; "+" stays.
QStringView bare(QStringView text)
{
    return text.size() > 1 && text.front() == u'\\' ? text.mid(1) : text;
}

} // namespace

const Symbol* symbol(QStringView text)
{
    const auto index = [](const auto& table) {
        QHash<QStringView, const Symbol*> map;
        for (const Symbol& s : table)
            map.insert(s.name, &s);
        return map;
    };
    static const QHash<QStringView, const Symbol*> commands = index(kSymbols);
    static const QHash<QStringView, const Symbol*> characters = index(kCharacters);
    // "\\{" and "\\|" are commands of one character, "|" a character.
    if (text.size() > 1 && text.front() == u'\\')
        return commands.value(text.mid(1), nullptr);
    return characters.value(text, nullptr);
}

Class classOf(QStringView text)
{
    if (const Symbol* s = symbol(text))
        return s->kind;
    const QStringView name = bare(text);
    if (text.startsWith(u'\\') && isFunctionName(name))
        return std::ranges::find(kLimitFunctions, name) != kLimitFunctions.end() ? Class::Limits : Class::Function;
    return Class::Ord;
}

bool isFunctionName(QStringView name)
{
    return std::ranges::find(kFunctions, name) != kFunctions.end()
        || std::ranges::find(kLimitFunctions, name) != kLimitFunctions.end();
}

bool takesLimits(QStringView command)
{
    const Class kind = classOf(command);
    return kind == Class::Op || kind == Class::Limits;
}

bool isIntegral(QStringView command)
{
    const QStringView name = bare(command);
    return name == u"int" || name == u"iint" || name == u"iiint" || name == u"oint" || name == u"oiint"
        || name == u"intop" || name == u"smallint";
}

const Accent* accent(QStringView command)
{
    const QStringView name = bare(command);
    const auto found = std::ranges::find(kAccents, name, &Accent::name);
    return found == kAccents.end() ? nullptr : &*found;
}

const Font* font(QStringView command)
{
    const QStringView name = bare(command);
    const auto found = std::ranges::find(kFonts, name, &Font::name);
    return found == kFonts.end() ? nullptr : &*found;
}

double spaceWidth(QStringView command)
{
    if (command == u"~")
        return 1.0 / 3;
    if (command.size() < 2 || command.front() != u'\\')
        return -1;
    const QStringView name = command.mid(1);
    const auto found = std::ranges::find(kSpaces, name, &Space::name);
    return found == kSpaces.end() ? -1 : found->em;
}

} // namespace ws::capture::latex
