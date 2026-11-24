# User manual (draft)

The site runs at <http://localhost:8080/> once it is installed (see `install.md`).
Screenshots will be added in phase 7.

## Customers

### Create an account or log in

Choose **Log in** at the top right.

- **New customers:** use **Create an account**. Your name, email and a password of at
  least 8 characters are all you need, and you are logged in straight away.
- **Existing customers:** use the **Log in** form. After five wrong passwords in a
  row, the account is locked for a short time.

### Find products

The **Shop** page lists every product, 12 per page.

- Type in the search box to match product names or SKUs.
- Pick a category to filter the list.
- Each card shows the price and stock status: *In stock*, *Only N left* or
  *Out of stock*.
- Open a product to read its description and choose a quantity.

### Cart

**Add to cart** puts one unit in your cart from the Shop page, or the chosen quantity
from a product page. The **Cart** link in the header shows how many items you have.

On the cart page you can:

- change a quantity, from 1 to 100;
- remove a line;
- see the running total.

Prices always come from the store, so they cannot be changed in the browser.

### Place an order

Choose **Place order** on the cart page.

- **If an item has run out:** the order is not placed, and a message names the product
  and how many are left.
- **If the order is placed:** you go to the order's tracking page.

### Track orders

**Orders** lists every order you have placed. Open one to see its items, its total
and a **Tracking** timeline of every status change. The page refreshes itself every
few seconds.

## Administrators

Log in with an admin account. An **Admin** link appears in the header.

### Products (Admin → Products)

- **Add a product:** fill in the SKU, name, price in rupees, category, opening stock,
  reorder level and description. The SKU may contain only capital letters, digits
  and `-`.
- **Edit:** choose **Edit** on a row, change the fields, then **Save changes**.
- **Delete:** this hides the product from the shop and removes it from every cart.
  Past orders still show it. **Restore** brings it back.
- **Adjust stock:** enter an amount, then choose a reason.
  - **Restock** adds the amount.
  - **Adjust** can add or remove, for example after breakage or a recount.
  - Stock can never go below zero.
- **Show only low-stock products** lists the products at or below their reorder
  level.

Every change is recorded in the audit log and the stock ledger.

### Orders (Admin → Order board)

Lists every order with its customer, status, item count and total. You can filter by
status, and the board refreshes itself every 5 seconds. Open an order to see its
timeline.

Packing, shipping, delivery, cancellation and refunds arrive in phase 3.
