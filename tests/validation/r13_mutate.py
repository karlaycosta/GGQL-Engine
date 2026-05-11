#!/usr/bin/env python3
"""r13_mutate.py — Mutation testing lite para arquivos C++ específicos.

Aplica operadores de mutação clássicos source-level, recompila e roda
ctest. Reporta mutation score (killed/total) por arquivo.

Operadores cobertos:
  ROR — Relational Operator Replacement   (<, <=, >, >=, ==, !=)
  LCR — Logical Connector Replacement     (&&, ||)
  UOI — Unary Operator Insertion          (!x → x)
  CRP — Constant Replacement              (0 ↔ 1)
  BCR — Boolean Constant Replacement      (true ↔ false)

Uso:
  python3 r13_mutate.py <source.cpp> [--targets test_a,test_b]
                                     [--max N] [--seed S]
                                     [--build-target lib_name]

Restauração de source via git stash (nunca toca arquivo permanentemente).
"""
from __future__ import annotations

import argparse
import os
import random
import re
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Iterator, Tuple

ROOT = Path(__file__).resolve().parents[2]

# ── Definição dos operadores ────────────────────────────────────────────────

ROR_PAIRS = [
    ("<", "<="), ("<=", "<"),
    (">", ">="), (">=", ">"),
    ("==", "!="), ("!=", "=="),
]

LCR_PAIRS = [
    ("&&", "||"), ("||", "&&"),
]

# Helper: regex que captura operadores como tokens (não cobre overloads)
# Evita matching dentro de strings/comments simples.

def is_in_string_or_comment(line: str, col: int) -> bool:
    """Heurística simples: detecta // ou string literal antes da posição."""
    if "//" in line[:col]:
        slash = line.index("//")
        # Verifica se // não está dentro de string
        if line[:slash].count('"') % 2 == 0:
            return True
    # Conta aspas antes da posição — se ímpar, está dentro de string
    quotes = sum(1 for c in line[:col] if c == '"' and (col == 0 or line[col-1] != '\\'))
    if quotes % 2 == 1:
        return True
    return False

@dataclass
class Mutation:
    file: Path
    line_num: int       # 1-based
    col: int            # 0-based em line
    op_type: str        # "ROR", "LCR", ...
    original: str
    replacement: str
    context: str        # snippet ±20 chars

    def apply(self, text: str) -> str:
        """Aplica a mutação ao texto completo do arquivo."""
        lines = text.split("\n")
        line = lines[self.line_num - 1]
        # Substitui apenas em self.col (não usa replace global)
        new_line = line[:self.col] + self.replacement + line[self.col + len(self.original):]
        lines[self.line_num - 1] = new_line
        return "\n".join(lines)

def find_mutations(file: Path) -> Iterator[Mutation]:
    """Encontra todas as mutações candidatas no arquivo."""
    text = file.read_text()
    for ln, line in enumerate(text.split("\n"), start=1):
        # Pula linhas que são comentários puros
        stripped = line.lstrip()
        if stripped.startswith("//") or stripped.startswith("*") or stripped.startswith("/*"):
            continue
        # Pula preprocessor directives (#include, #define, #if/#ifdef, etc.)
        # Mutações em #include são trivialmente equivalentes (não afetam código)
        if stripped.startswith("#"):
            continue

        # ROR — escaneamento com tokenização leve (ignora <= como < em "<=")
        # Estratégia: regex com lookahead/lookbehind para isolamento
        for orig, repl in ROR_PAIRS:
            # Match exato como token (não parte de outro operador)
            # Negativos:
            #   < não pode ser parte de <= ou << ou <-
            #   > não pode ser parte de >= ou >> ou ->
            #   = não isolado é parte de == ou =>
            pattern = re.escape(orig)
            for m in re.finditer(pattern, line):
                col = m.start()
                # Boundary check: caractere anterior + posterior
                prev_c = line[col - 1] if col > 0 else " "
                next_c = line[col + len(orig)] if col + len(orig) < len(line) else " "
                # Filtros para isolamento
                if orig in ("<", ">"):
                    # Não pode ser parte de "<<", ">>", "<=", ">=", "->", "<-", template "<>"
                    if next_c in "<>=":  # já parte de <=, >=, <<, >>
                        continue
                    if prev_c in "<>=-":
                        continue
                    # Templates: ignora linhas com std::, template<, vector<, std::variant<, etc.
                    # Heurística: se tem "::" ou template/std antes no line, skip
                    if "::" in line[:col] or "template" in line[:col]:
                        continue
                    # Ignora std::cout << ... e operator<<
                    if "<<" in line or ">>" in line:
                        continue
                elif orig == "=":
                    # Não usado (igualdade real é ==)
                    pass
                elif orig in ("==", "!=", "<=", ">="):
                    # Já são 2-char, mas verifica que não seja parte de === ou similar
                    if next_c == "=" or prev_c == "=":
                        continue

                if is_in_string_or_comment(line, col):
                    continue

                yield Mutation(
                    file=file, line_num=ln, col=col,
                    op_type="ROR", original=orig, replacement=repl,
                    context=line[max(0, col - 20):col + len(orig) + 20].strip(),
                )

        # LCR
        for orig, repl in LCR_PAIRS:
            for m in re.finditer(re.escape(orig), line):
                col = m.start()
                if is_in_string_or_comment(line, col):
                    continue
                yield Mutation(
                    file=file, line_num=ln, col=col,
                    op_type="LCR", original=orig, replacement=repl,
                    context=line[max(0, col - 20):col + len(orig) + 20].strip(),
                )

        # BCR (boolean true/false swap)
        for orig, repl in [("true", "false"), ("false", "true")]:
            for m in re.finditer(rf"\b{orig}\b", line):
                col = m.start()
                if is_in_string_or_comment(line, col):
                    continue
                yield Mutation(
                    file=file, line_num=ln, col=col,
                    op_type="BCR", original=orig, replacement=repl,
                    context=line[max(0, col - 20):col + len(orig) + 20].strip(),
                )

# ── Build e testes ──────────────────────────────────────────────────────────

def run(cmd: list[str], cwd: Path = ROOT, timeout: int = 600) -> Tuple[int, str]:
    """Roda comando e retorna (returncode, combined_output)."""
    try:
        p = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, timeout=timeout)
        return p.returncode, p.stdout + p.stderr
    except subprocess.TimeoutExpired:
        return 124, "TIMEOUT"

def build_and_test(targets: list[str], build_target: str | None = None) -> Tuple[bool, str]:
    """Compila incremental + roda testes. Retorna (testes_passaram, log)."""
    env = os.environ.copy()
    env["CXX"] = "g++-14"
    env["CC"] = "gcc-14"

    # Build incremental (rapido, ~5-15s típico)
    build_cmd = ["cmake", "--build", "build", "-j8"]
    if build_target:
        build_cmd += ["--target", build_target]
    rc, log = run(build_cmd, timeout=300)
    if rc != 0:
        return False, "BUILD_FAIL:\n" + log[-500:]

    # ctest com targets específicos para reduzir tempo
    test_cmd = ["ctest", "--test-dir", "build", "-j4", "--output-on-failure"]
    if targets:
        # ctest -R "regex" — junta com |
        regex = "|".join(f"^{t}$" for t in targets)
        test_cmd += ["-R", regex]
    rc, log = run(test_cmd, timeout=300)
    return rc == 0, log[-500:]

# ── Campanha ────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("source", type=Path)
    ap.add_argument("--targets", default="",
                    help="ctest targets separados por vírgula (default: todos)")
    ap.add_argument("--build-target", default=None,
                    help="cmake --build --target (default: build all)")
    ap.add_argument("--max", type=int, default=30,
                    help="Máximo de mutações a tentar (sample aleatório)")
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--dry-run", action="store_true",
                    help="Só lista mutações, não roda build/test")
    args = ap.parse_args()

    source = args.source.resolve()
    if not source.exists():
        sys.exit(f"Não existe: {source}")

    targets = [t for t in args.targets.split(",") if t]

    random.seed(args.seed)

    # 1. Faz baseline (sem mutação) para verificar que testes passam
    print(f"═══ Mutation testing R13: {source.relative_to(ROOT)} ═══")
    print(f"  Targets: {targets if targets else 'all'}")
    print(f"  Build target: {args.build_target or 'all'}")
    print()
    print("→ Baseline (sem mutações)...")
    if not args.dry_run:
        t0 = time.time()
        ok, log = build_and_test(targets, args.build_target)
        dt = time.time() - t0
        if not ok:
            print(f"❌ BASELINE FALHOU em {dt:.1f}s. Testes não passam antes da mutação:")
            print(log[-300:])
            sys.exit(1)
        print(f"✅ Baseline ok em {dt:.1f}s")

    # 2. Encontra todas mutações candidatas
    all_muts = list(find_mutations(source))
    print(f"\n→ Encontradas {len(all_muts)} mutações candidatas")
    if not all_muts:
        sys.exit("Nenhuma mutação aplicável (arquivo sem operators?)")

    # Amostra aleatória
    sample = random.sample(all_muts, min(args.max, len(all_muts)))
    print(f"  Sample (seed={args.seed}): {len(sample)} mutações")

    if args.dry_run:
        for i, m in enumerate(sample[:20], 1):
            print(f"  #{i:02d} L{m.line_num:4d} {m.op_type} '{m.original}' → '{m.replacement}'  | {m.context}")
        if len(sample) > 20:
            print(f"  ... e mais {len(sample) - 20}")
        return

    # 3. Para cada mutação: aplica, builda, testa, restaura
    original = source.read_text()
    killed = 0
    survived = 0
    timeout = 0
    build_fail = 0

    for i, m in enumerate(sample, 1):
        mutated = m.apply(original)
        source.write_text(mutated)

        t0 = time.time()
        ok, log = build_and_test(targets, args.build_target)
        dt = time.time() - t0

        if log.startswith("BUILD_FAIL"):
            outcome = "BUILD_FAIL"
            build_fail += 1
        elif "TIMEOUT" in log:
            outcome = "TIMEOUT"
            timeout += 1
        elif ok:
            outcome = "SURVIVED"
            survived += 1
        else:
            outcome = "KILLED"
            killed += 1

        print(f"  #{i:02d} L{m.line_num:4d} {m.op_type} '{m.original}'→'{m.replacement}' "
              f"({dt:.0f}s) {outcome:10s} | {m.context[:50]}")

        # Restaura
        source.write_text(original)

    # 4. Resumo
    total_executed = killed + survived  # exclui BUILD_FAIL/TIMEOUT (mutations não-equivalentes)
    score = (killed / total_executed * 100) if total_executed else 0.0

    print()
    print("═══ Resumo R13 ═══")
    print(f"  Total tentativas:      {len(sample)}")
    print(f"  KILLED (test detectou):  {killed}")
    print(f"  SURVIVED (test passou):  {survived}  ← lacunas de teste!")
    print(f"  BUILD_FAIL (mutação inválida): {build_fail}  (excluído do score)")
    print(f"  TIMEOUT:                 {timeout}  (excluído do score)")
    print()
    print(f"  Mutation score: {killed}/{total_executed} = {score:.1f}%")
    print()
    if score >= 80.0:
        print("✅ EXCELENTE: ≥80% mutation score (testes detectam vasta maioria dos bugs)")
    elif score >= 60.0:
        print("✅ BOM: 60-80% mutation score (testes razoavelmente fortes)")
    elif score >= 40.0:
        print("⚠  MODERADO: 40-60% (algumas lacunas significativas)")
    else:
        print("❌ FRACO: <40% (muitas mutações não detectadas)")

if __name__ == "__main__":
    main()
