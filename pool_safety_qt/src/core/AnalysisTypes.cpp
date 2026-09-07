#include "core/AnalysisTypes.h"

namespace core {

QString DangerReport::advice() const
{
    return situationAction(situation);
}

QString DangerReport::describe() const
{
    QString text = situationText(situation);
    if (!reasons.isEmpty())
        text += QStringLiteral(": ") + reasons.join(QStringLiteral("; "));
    return text;
}


const PersonView *matchPrevious(const QVector<PersonView> &previous, const QRect &box)
{
    const PersonView *best = nullptr;
    double bestShare = 0.0;

    for (const PersonView &person : previous) {
        const QRect common = person.box.intersected(box);
        if (common.isEmpty())
            continue;

        const double area = double(box.width()) * box.height();
        if (area <= 0.0)
            continue;

        const double share = double(common.width()) * common.height() / area;
        if (share > bestShare) {
            bestShare = share;
            best = &person;
        }
    }

    // Половина рамки — уверенное совпадение. Меньше — скорее сосед, стоящий
    // рядом, и брать его вердикт нельзя.
    return bestShare >= 0.5 ? best : nullptr;
}

} // namespace core
